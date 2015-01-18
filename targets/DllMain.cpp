#include "Main.h"
#include "Thread.h"
#include "DllHacks.h"
#include "DllOverlayUi.h"
#include "NetplayManager.h"
#include "ChangeMonitor.h"
#include "SmartSocket.h"
#include "UdpSocket.h"
#include "Exceptions.h"
#include "Enum.h"
#include "ErrorStringsExt.h"
#include "KeyboardState.h"
#include "CharacterSelect.h"
#include "SpectatorManager.h"
#include "DllControllerManager.h"
#include "DllFrameRate.h"
#include "ReplayManager.h"

#include <windows.h>

#include <vector>
#include <memory>
#include <algorithm>

using namespace std;


// The main log file path
#define LOG_FILE                    FOLDER "dll.log"

// The number of milliseconds to poll for events each frame
#define POLL_TIMEOUT                ( 3 )

// The extra number of frames to delay checking round over state during rollback
#define ROLLBACK_ROUND_OVER_DELAY   ( 5 )

// The number of milliseconds to wait for the initial connect
#define INITIAL_CONNECT_TIMEOUT     ( 30000 )

// The number of milliseconds to wait to perform a delayed stop so that ErrorMessages are received before sockets die
#define DELAYED_STOP                ( 100 )

// The number of milliseconds before resending inputs while waiting for more inputs
#define RESEND_INPUTS_INTERVAL      ( 100 )

// The maximum number of milliseconds to wait for inputs before timeout
#define MAX_WAIT_INPUTS_INTERVAL    ( 10000 )

// The maximum number of spectators allowed for ClientMode::Spectate
#define MAX_SPECTATORS              ( 15 )

// The maximum number of spectators allowed for ClientMode::Host/Client
#define MAX_ROOT_SPECTATORS         ( 1 )

// Indicates if this client should redirect spectators
#define SHOULD_REDIRECT_SPECTATORS  ( clientMode.isSpectate()                                                       \
                                      ? numSpectators() >= MAX_SPECTATORS                                           \
                                      : numSpectators() >= MAX_ROOT_SPECTATORS )


#define LOG_SYNC(FORMAT, ...)                                                                                       \
    LOG_TO ( syncLog, "%s [%u] %s [%s] " FORMAT,                                                                    \
             gameModeStr ( *CC_GAME_MODE_ADDR ), *CC_GAME_MODE_ADDR,                                                \
             netMan.getState(), netMan.getIndexedFrame(), ## __VA_ARGS__ )

#define LOG_SYNC_CHARACTER(N)                                                                                       \
    LOG_SYNC ( "P%u: C=%u; M=%u; c=%u; seq=%u; st=%u; hp=%u; rh=%u; gb=%.1f; gq=%.1f; mt=%u; ht=%u; x=%d; y=%d",    \
               N, *CC_P ## N ## _CHARACTER_ADDR, *CC_P ## N ## _MOON_SELECTOR_ADDR,                                 \
               *CC_P ## N ## _COLOR_SELECTOR_ADDR, *CC_P ## N ## _SEQUENCE_ADDR, *CC_P ## N ## _SEQ_STATE_ADDR,     \
               *CC_P ## N ## _HEALTH_ADDR, *CC_P ## N ## _RED_HEALTH_ADDR, *CC_P ## N ## _GUARD_BAR_ADDR,           \
               *CC_P ## N ## _GUARD_QUALITY_ADDR,  *CC_P ## N ## _METER_ADDR, *CC_P ## N ## _HEAT_ADDR,             \
               *CC_P ## N ## _X_POSITION_ADDR, *CC_P ## N ## _Y_POSITION_ADDR )


// Main application state
static ENUM ( AppState, Uninitialized, Polling, Stopping, Deinitialized ) appState = AppState::Uninitialized;

// Main application instance
struct DllMain;
static shared_ptr<DllMain> main;

// Mutex for deinitialize()
static Mutex deinitMutex;
static void deinitialize();

// Enum of variables to monitor
ENUM ( Variable, WorldTime, GameMode, SkippableFlag, IntroState,
       MenuConfirmState, AutoReplaySave, GameStateCounter, CurrentMenuIndex );


struct DllMain
        : public Main
        , public RefChangeMonitor<Variable, uint8_t>::Owner
        , public RefChangeMonitor<Variable, uint32_t>::Owner
        , public PtrToRefChangeMonitor<Variable, uint32_t>::Owner
        , public SpectatorManager
        , public DllControllerManager
{
    // NetplayManager instance
    NetplayManager netMan;

    // If remote has loaded up to character select
    bool remoteCharaSelectLoaded = false;

    // ChangeMonitor for CC_WORLD_TIMER_ADDR
    RefChangeMonitor<Variable, uint32_t> worldTimerMoniter;

    // Timer for resending inputs while waiting
    TimerPtr resendTimer;

    // Timer for waiting for inputs
    int waitInputsTimer = -1;

    // Indicates if we should sync the game RngState on this frame
    bool shouldSyncRngState = false;

    // Frame to stop on, when fast-forwarding the game.
    // Used as a flag to indicate fast-forward mode, 0:0 means not fast-forwarding.
    IndexedFrame fastFwdStopFrame = {{ 0, 0 }};

    // Initial connect timer
    TimerPtr initialTimer;

    // Local player inputs
    array<uint16_t, 2> localInputs = {{ 0, 0 }};

    // If we have sent our local retry menu index
    bool localRetryMenuIndexSent = false;

    // If we should disconnect at the next NetplayState change
    bool lazyDisconnect = false;

    // If the delay and/or rollback should be changed
    bool shouldChangeDelayRollback = false;

    // Latest ChangeConfig for changing delay/rollback
    ChangeConfig changeConfig;

    // Client serverCtrlSocket address
    IpAddrPort clientServerAddr;

    // Sockets that have been redirected to another client
    unordered_set<Socket *> redirectedSockets;

    // Timer to delay checking round over state during rollback
    int roundOverTimer = -1;

#ifndef RELEASE
    // Local and remote SyncHashes
    list<MsgPtr> localSync, remoteSync;

    // Debug testing flags
    bool randomInputs = false;
    bool randomDelay = false;
    bool randomRollback = false;
    uint32_t rollUpTo = 10;
    bool replayInputs = false;

    // ReplayManager instance
    ReplayManager repMan;
    IndexedFrame replayStop = MaxIndexedFrame;
#endif // RELEASE

    void frameStepNormal()
    {
        switch ( netMan.getState().value )
        {
            case NetplayState::PreInitial:
            case NetplayState::Initial:
            case NetplayState::AutoCharaSelect:
                // Disable FPS limit while going to character select
                *CC_SKIP_FRAMES_ADDR = 1;
                break;

            case NetplayState::InGame:
                if ( netMan.config.rollback )
                {
                    // Only save rollback states in-game
                    procMan.saveState ( netMan );

                    // Delayed round over check
                    if ( roundOverTimer == 0 )
                        checkRoundOver();

                    if ( roundOverTimer > 0 )
                        --roundOverTimer;
                }

            case NetplayState::CharaSelect:
            case NetplayState::Loading:
            case NetplayState::Skippable:
            case NetplayState::RetryMenu:
            {
                // Fast forward if spectator
                if ( clientMode.isSpectate() && netMan.getState() != NetplayState::Loading )
                {
                    static bool doneSkipping = true;

                    const IndexedFrame remoteIndexedFrame = netMan.getRemoteIndexedFrame();

                    if ( doneSkipping && remoteIndexedFrame.value > netMan.getIndexedFrame().value + NUM_INPUTS )
                    {
                        *CC_SKIP_FRAMES_ADDR = 1;
                        doneSkipping = false;
                    }
                    else if ( !doneSkipping && *CC_SKIP_FRAMES_ADDR == 0 )
                    {
                        doneSkipping = true;
                    }
                }

                ASSERT ( localPlayer == 1 || localPlayer == 2 );

                checkOverlay ( netMan.getState() == NetplayState::CharaSelect || clientMode.isNetplay() );

                KeyboardState::update();
                ControllerManager::get().check();

                if ( DllOverlayUi::isEnabled() )                // Overlay UI input
                {
                    localInputs[0] = localInputs[1] = 0;
                }
                else if ( clientMode.isNetplay() )              // Netplay input
                {
                    if ( playerControllers[localPlayer - 1] )
                        localInputs[0] = getInput ( playerControllers[localPlayer - 1] );

                    if ( KeyboardState::isDown ( VK_CONTROL ) )
                    {
                        for ( uint8_t delay = 0; delay < 10; ++delay )
                        {
                            if ( delay == netMan.getDelay() )
                                continue;

                            if ( KeyboardState::isPressed ( '0' + delay )                   // Ctrl + Number
                                    || KeyboardState::isPressed ( VK_NUMPAD0 + delay ) )    // Ctrl + Numpad Number
                            {
                                shouldChangeDelayRollback = true;
                                changeConfig.indexedFrame = netMan.getIndexedFrame();
                                changeConfig.delay = delay;
                                changeConfig.invalidate();
                                dataSocket->send ( changeConfig );
                                break;
                            }
                        }
                    }

                    // TODO Alt+Number to change rollback

#ifndef RELEASE
                    // Test random delay setting
                    if ( KeyboardState::isPressed ( VK_F11 ) )
                    {
                        randomDelay = !randomDelay;
                        DllOverlayUi::showMessage ( randomDelay ? "Enabled random delay" : "Disabled random delay" );
                    }

                    if ( randomDelay && rand() % 30 == 0 )
                    {
                        shouldChangeDelayRollback = true;
                        changeConfig.indexedFrame = netMan.getIndexedFrame();
                        changeConfig.delay = rand() % 10;
                        changeConfig.invalidate();
                        dataSocket->send ( changeConfig );
                    }
#endif // RELEASE
                }
                else if ( clientMode.isLocal() )                // Local input
                {
                    if ( playerControllers[localPlayer - 1] )
                        localInputs[0] = getInput ( playerControllers[localPlayer - 1] );
                }
                else if ( clientMode.isSpectate() )             // Spectator input
                {
                    if ( KeyboardState::isDown ( VK_SPACE ) )
                        *CC_SKIP_FRAMES_ADDR = 0;
                }
                else
                {
                    LOG ( "Unknown clientMode=%s; flags={ %s }", clientMode, clientMode.flagString() );
                    break;
                }

#ifndef RELEASE
                // Replay inputs and rollback
                if ( replayInputs )
                {
                    if ( repMan.getGameMode ( netMan.getIndexedFrame() ) )
                        ASSERT ( repMan.getGameMode ( netMan.getIndexedFrame() ) == *CC_GAME_MODE_ADDR );

                    if ( !repMan.getStateStr ( netMan.getIndexedFrame() ).empty() )
                        ASSERT ( repMan.getStateStr ( netMan.getIndexedFrame() ) == netMan.getState().str() );

                    // Inputs
                    const auto& inputs = repMan.getInputs ( netMan.getIndexedFrame() );
                    netMan.setInput ( 1, inputs.p1 );
                    netMan.setInput ( 2, inputs.p2 );

                    const IndexedFrame target = repMan.getRollbackTarget ( netMan.getIndexedFrame() );

                    // Rollback
                    if ( netMan.isInRollback() && target.value < netMan.getIndexedFrame().value )
                    {
                        // Reinputs
                        const auto& reinputs = repMan.getReinputs ( netMan.getIndexedFrame() );
                        for ( const auto& inputs : reinputs )
                        {
                            netMan.assignInput ( 1, inputs.p1, inputs.indexedFrame );
                            netMan.assignInput ( 2, inputs.p2, inputs.indexedFrame );
                        }

                        const string before = format ( "%s [%u] %s [%s]",
                                                       gameModeStr ( *CC_GAME_MODE_ADDR ), *CC_GAME_MODE_ADDR,
                                                       netMan.getState(), netMan.getIndexedFrame() );

                        // Indicate we're re-running to the current frame
                        fastFwdStopFrame = netMan.getIndexedFrame();

                        // Reset the game state (this resets game state AND netMan state)
                        if ( procMan.loadState ( target, netMan ) )
                        {
                            // Start fast-forwarding now
                            *CC_SKIP_FRAMES_ADDR = 1;

                            LOG_TO ( syncLog, "%s Rollback: target=[%s]; actual=[%s]",
                                     before, target, netMan.getIndexedFrame() );

                            LOG_SYNC ( "Reinputs: 0x%04x 0x%04x", netMan.getRawInput ( 1 ), netMan.getRawInput ( 2 ) );
                            return;
                        }

                        LOG_TO ( syncLog, "%s Rollback to target=[%s] failed!", before, target );

                        ASSERT_IMPOSSIBLE;
                    }

                    // RngState
                    if ( netMan.getFrame() == 0 && ( netMan.getState() == NetplayState::CharaSelect
                                                     || netMan.getState() == NetplayState::InGame ) )
                    {
                        MsgPtr msgRngState = repMan.getRngState ( netMan.getIndexedFrame() );

                        if ( msgRngState )
                            procMan.setRngState ( msgRngState->getAs<RngState>() );
                    }

                    break;
                }

                // Test random input
                if ( KeyboardState::isPressed ( VK_F12 ) )
                {
                    randomInputs = !randomInputs;
                    localInputs [ clientMode.isLocal() ? 1 : 0 ] = 0;
                    DllOverlayUi::showMessage ( randomInputs ? "Enabled random inputs" : "Disabled random inputs" );
                }

                if ( randomInputs )
                {
                    bool shouldRandomize = ( netMan.getFrame() % 2 );
                    if ( netMan.isInRollback() )
                        shouldRandomize = ( netMan.getFrame() % 150 < 50 );

                    if ( shouldRandomize )
                    {
                        uint16_t direction = ( rand() % 10 );

                        // Reduce the chances of moving the cursor at retry menu
                        if ( netMan.getState() == NetplayState::RetryMenu && ( rand() % 2 ) )
                            direction = 0;

                        uint16_t buttons = ( rand() % 0x1000 );

                        // Prevent hitting some non-essential buttons
                        buttons &= ~ ( CC_BUTTON_FN1 | CC_BUTTON_FN2 | CC_BUTTON_START );

                        // Prevent going back at character select
                        if ( netMan.getState() == NetplayState::CharaSelect )
                            buttons &= ~ ( CC_BUTTON_B | CC_BUTTON_CANCEL );

                        localInputs [ clientMode.isLocal() ? 1 : 0 ] = COMBINE_INPUT ( direction, buttons );
                    }
                }
#endif // RELEASE

                // Assign local player input
                if ( !clientMode.isSpectate() )
                {
#ifndef RELEASE
                    if ( netMan.isInRollback() )
                        netMan.assignInput ( localPlayer, localInputs[0], netMan.getFrame() + netMan.getDelay() );
                    else
#endif // RELEASE
                        netMan.setInput ( localPlayer, localInputs[0] );
                }

                if ( clientMode.isNetplay() )
                {
                    // Special netplay retry menu behaviour, only select final option after both sides have selected
                    if ( netMan.getState() == NetplayState::RetryMenu )
                    {
                        MsgPtr msgMenuIndex = netMan.getLocalRetryMenuIndex();

                        // Lazy disconnect now once the retry menu option has been selected
                        if ( msgMenuIndex && ( !dataSocket || !dataSocket->isConnected() ) )
                        {
                            if ( lazyDisconnect )
                            {
                                lazyDisconnect = false;
                                delayedStop ( "Disconnected!" );
                            }
                            break;
                        }

                        // Only send retry menu index once
                        if ( msgMenuIndex && !localRetryMenuIndexSent )
                        {
                            localRetryMenuIndexSent = true;
                            dataSocket->send ( msgMenuIndex );
                        }
                        break;
                    }

                    dataSocket->send ( netMan.getInputs ( localPlayer ) );
                }
                else if ( clientMode.isLocal() )
                {
                    if ( playerControllers[remotePlayer - 1] && !DllOverlayUi::isEnabled() )
                        localInputs[1] = getInput ( playerControllers[remotePlayer - 1] );

                    netMan.setInput ( remotePlayer, localInputs[1] );
                }

                if ( shouldSyncRngState && ( clientMode.isHost() || clientMode.isBroadcast() ) )
                {
                    shouldSyncRngState = false;

                    MsgPtr msgRngState = procMan.getRngState ( netMan.getIndex() );

                    ASSERT ( msgRngState.get() != 0 );

                    netMan.setRngState ( msgRngState->getAs<RngState>() );

                    if ( clientMode.isHost() )
                        dataSocket->send ( msgRngState );
                }
                break;
            }

            default:
                ASSERT ( !"Unknown NetplayState!" );
                break;
        }

        // Clear the last changed frame before we get new inputs
        netMan.clearLastChangedFrame();

        for ( ;; )
        {
            // Poll until we are ready to run
            if ( !EventManager::get().poll ( POLL_TIMEOUT ) )
            {
                appState = AppState::Stopping;
                return;
            }

            // Don't need to wait for anything in local modes
            if ( clientMode.isLocal() || lazyDisconnect )
                break;

            // Check if we are ready to continue running, ie not waiting on remote input or RngState
            const bool ready = ( netMan.isRemoteInputReady() && netMan.isRngStateReady ( shouldSyncRngState ) );

            // Don't resend inputs in spectator mode
            if ( clientMode.isSpectate() )
            {
                // Continue if ready
                if ( ready )
                    break;
            }
            else
            {
                // Stop resending inputs if we're ready
                if ( ready )
                {
                    resendTimer.reset();
                    waitInputsTimer = -1;
                    break;
                }

                // Start resending inputs since we are waiting
                if ( !resendTimer )
                {
                    resendTimer.reset ( new Timer ( this ) );
                    resendTimer->start ( RESEND_INPUTS_INTERVAL );
                    waitInputsTimer = 0;
                }
            }
        }

#ifndef RELEASE
        if ( !replayInputs )
        {
            // Test one time rollback
            if ( KeyboardState::isPressed ( VK_F9 ) && netMan.isInGame() )
            {
                IndexedFrame target = netMan.getIndexedFrame();

                if ( target.parts.frame <= 30 )
                    target.parts.frame = 0;
                else
                    target.parts.frame -= 30;

                procMan.loadState ( target, netMan );
            }

            // Test random rollback
            if ( KeyboardState::isPressed ( VK_F10 ) )
            {
                randomRollback = !randomRollback;
                DllOverlayUi::showMessage ( randomRollback ? "Enabled random rollback" : "Disabled random rollback" );
            }

            if ( randomRollback && netMan.isInGame() && ( netMan.getFrame() % 150 < 50 ) )
            {
                const uint32_t distance = 1 + ( rand() % rollUpTo );

                IndexedFrame target = netMan.getIndexedFrame();

                if ( target.parts.frame <= distance )
                    target.parts.frame = 0;
                else
                    target.parts.frame -= distance;

                const string before = format ( "%s [%u] %s [%s]",
                                               gameModeStr ( *CC_GAME_MODE_ADDR ), *CC_GAME_MODE_ADDR,
                                               netMan.getState(), netMan.getIndexedFrame() );

                // Indicate we're re-running to the current frame
                fastFwdStopFrame = netMan.getIndexedFrame();

                // Reset the game state (this resets game state AND netMan state)
                if ( procMan.loadState ( target, netMan ) )
                {
                    // Start fast-forwarding now
                    *CC_SKIP_FRAMES_ADDR = 1;

                    LOG_TO ( syncLog, "%s Rollback: target=[%s]; actual=[%s]",
                             before, target, netMan.getIndexedFrame() );

                    LOG_SYNC ( "Reinputs: 0x%04x 0x%04x", netMan.getRawInput ( 1 ), netMan.getRawInput ( 2 ) );
                    return;
                }

                LOG_TO ( syncLog, "%s Rollback to target=[%s] failed!", before, target );
            }
        }
#endif // RELEASE

        // Only rollback when necessary
        if ( netMan.isInRollback() && netMan.getLastChangedFrame().value < netMan.getIndexedFrame().value )
        {
            const string before = format ( "%s [%u] %s [%s]",
                                           gameModeStr ( *CC_GAME_MODE_ADDR ), *CC_GAME_MODE_ADDR,
                                           netMan.getState(), netMan.getIndexedFrame() );

            // Indicate we're re-running to the current frame
            fastFwdStopFrame = netMan.getIndexedFrame();

            // Reset the game state (this resets game state AND netMan state)
            if ( procMan.loadState ( netMan.getLastChangedFrame(), netMan ) )
            {
                // Start fast-forwarding now
                *CC_SKIP_FRAMES_ADDR = 1;

                LOG_TO ( syncLog, "%s Rollback: target=[%s]; actual=[%s]",
                         before, netMan.getLastChangedFrame(), netMan.getIndexedFrame() );

                LOG_SYNC ( "Reinputs: 0x%04x 0x%04x", netMan.getRawInput ( 1 ), netMan.getRawInput ( 2 ) );
                return;
            }

            LOG_TO ( syncLog, "%s Rollback to target=[%s] failed!", before, netMan.getLastChangedFrame() );
        }

        // Update the RngState if necessary
        if ( shouldSyncRngState )
        {
            shouldSyncRngState = false;

            // LOG ( "Randomizing RngState" );

            // RngState rngState;

            // rngState.rngState0 = rand() % 1000000;
            // rngState.rngState1 = rand() % 1000000;
            // rngState.rngState2 = rand() % 1000000;

            // for ( char& c : rngState.rngState3 )
            //     c = rand() % 256;

            // procMan.setRngState ( rngState );

            MsgPtr msgRngState = netMan.getRngState();

            if ( msgRngState )
                procMan.setRngState ( msgRngState->getAs<RngState>() );
        }

        // Update delay and/or rollback if necessary
        if ( shouldChangeDelayRollback )
        {
            shouldChangeDelayRollback = false;

            if ( changeConfig.delay != 0xFF && changeConfig.delay != netMan.getDelay() )
            {
                LOG ( "Delayed was changed %u -> %u", netMan.getDelay(), changeConfig.delay );
                DllOverlayUi::showMessage ( format ( "Delay was changed to %u", changeConfig.delay ) );
                netMan.setDelay ( changeConfig.delay );
                procMan.ipcSend ( changeConfig );
            }

            // TODO set rollback
        }

        const int delta = netMan.getRemoteFrameDelta();

        if ( delta < 0 )
            DllFrameRate::desiredFps = 61;
        else
            DllFrameRate::desiredFps = 60;

#ifndef RELEASE
        if ( dataSocket && dataSocket->isConnected()
                && ( ( netMan.getFrame() % ( 5 * 60 ) == 0 ) || ( netMan.getFrame() % 150 == 149 ) )
                && netMan.getState().value >= NetplayState::CharaSelect && netMan.getState() != NetplayState::Loading
                && netMan.getState() != NetplayState::Skippable && netMan.getState() != NetplayState::RetryMenu )
        {
            // Check for desyncs by periodically sending hashes
            if ( !netMan.isInRollback() || ( netMan.getFrame() == 0 ) || ( netMan.getFrame() % 150 == 149 ) )
            {
                MsgPtr msgSyncHash ( new SyncHash ( netMan.getIndexedFrame() ) );
                dataSocket->send ( msgSyncHash );
                localSync.push_back ( msgSyncHash );
            }
        }

        // Compare current lists of sync hashes
        while ( !localSync.empty() && !remoteSync.empty() )
        {

#define L localSync.front()->getAs<SyncHash>()
#define R remoteSync.front()->getAs<SyncHash>()

            while ( !remoteSync.empty() && L.indexedFrame.value > R.indexedFrame.value )
                remoteSync.pop_front();

            if ( remoteSync.empty() )
                break;

            while ( !localSync.empty() && R.indexedFrame.value > L.indexedFrame.value )
                localSync.pop_front();

            if ( localSync.empty() )
                break;

            if ( L == R )
            {
                localSync.pop_front();
                remoteSync.pop_front();
                continue;
            }

            LOG_TO ( syncLog, "Desync:" );
            LOG_TO ( syncLog, "< %s", L.dump() );
            LOG_TO ( syncLog, "> %s", R.dump() );

#undef L
#undef R

            syncLog.deinitialize();
            delayedStop ( "Desync!" );

            randomInputs = false;
            localInputs [ clientMode.isLocal() ? 1 : 0 ] = 0;
            return;
        }

        DllOverlayUi::debugText = format ( "%+d [%s]", delta, netMan.getIndexedFrame() );
        DllOverlayUi::debugTextAlign = 1;

        if ( !KeyboardState::isDown ( VK_SPACE ) && replayInputs && netMan.getIndex() <= repMan.getLastIndex() )
            *CC_SKIP_FRAMES_ADDR = 1;

        if ( netMan.getIndex() == repMan.getLastIndex() && netMan.getFrame() == repMan.getLastFrame() )
        {
            replayInputs = false;
            SetForegroundWindow ( ( HWND ) DllHacks::windowHandle );
        }
#endif // RELEASE

        // Cleared last played sound effects
        memset ( AsmHacks::sfxFilterArray, 0, CC_SFX_ARRAY_LEN );

#ifndef DISABLE_LOGGING
        MsgPtr msgRngState = procMan.getRngState ( 0 );
        ASSERT ( msgRngState.get() != 0 );

        // Log state every frame
        LOG_SYNC ( "RngState: %s", msgRngState->getAs<RngState>().dump() );
        LOG_SYNC ( "Inputs: 0x%04x 0x%04x", netMan.getRawInput ( 1 ), netMan.getRawInput ( 2 ) );

#ifndef RELEASE
        if ( netMan.getIndexedFrame().value == replayStop.value )
            MessageBox ( 0, 0, 0, 0 );
#endif // RELEASE

        // Log extra state during chara select
        if ( netMan.getState() == NetplayState::CharaSelect )
        {
            LOG_SYNC ( "P1: sel=%u; C=%u; M=%u; c=%u; P2: sel=%u; C=%u; M=%u; c=%u",
                       *CC_P1_SELECTOR_MODE_ADDR, *CC_P1_CHARACTER_ADDR,
                       *CC_P1_MOON_SELECTOR_ADDR, *CC_P1_COLOR_SELECTOR_ADDR,
                       *CC_P2_SELECTOR_MODE_ADDR, *CC_P2_CHARACTER_ADDR,
                       *CC_P2_MOON_SELECTOR_ADDR, *CC_P2_COLOR_SELECTOR_ADDR );
            return;
        }

        // Log extra state while in-game
        if ( netMan.getState() == NetplayState::InGame )
        {
            LOG_SYNC_CHARACTER ( 1 );
            LOG_SYNC_CHARACTER ( 2 );
            LOG_SYNC ( "roundOverTimer=%d; introState=%u; roundTimer=%u; realTimer=%u; hitsparks=%u; camera={ %d, %d }",
                       roundOverTimer, *CC_INTRO_STATE_ADDR, *CC_ROUND_TIMER_ADDR, *CC_REAL_TIMER_ADDR,
                       *CC_HIT_SPARKS_ADDR, *CC_CAMERA_X_ADDR, *CC_CAMERA_Y_ADDR );
            return;
        }
#endif // DISABLE_LOGGING
    }

    void frameStepRerun()
    {
        // We don't save any states while re-running because the inputs are faked

        // Stop fast-forwarding once we're reached the frame we want
        if ( netMan.getIndexedFrame().value >= fastFwdStopFrame.value )
            fastFwdStopFrame.value = 0;

        // Disable FPS limit only while fast-forwarding
        *CC_SKIP_FRAMES_ADDR = ( fastFwdStopFrame.value ? 1 : 0 );

#ifndef RELEASE
        if ( replayInputs )
            *CC_SKIP_FRAMES_ADDR = 1;
#endif // RELEASE

        LOG_SYNC ( "Reinputs: 0x%04x 0x%04x", netMan.getRawInput ( 1 ), netMan.getRawInput ( 2 ) );
    }

    void frameStep()
    {
        // New frame
        netMan.updateFrame();
        procMan.clearInputs();

        // Check for changes to important variables for state transitions
        ChangeMonitor::get().check();

        // Need to manually set the intro state to 0 during rollback
        if ( netMan.isInGame() && netMan.getFrame() > 224 && *CC_INTRO_STATE_ADDR )
            *CC_INTRO_STATE_ADDR = 0;

        // Perform the frame step
        if ( fastFwdStopFrame.value )
            frameStepRerun();
        else
            frameStepNormal();

        // Update spectators
        frameStepSpectators();

        // Write game inputs
        procMan.writeGameInput ( localPlayer, netMan.getInput ( localPlayer ) );
        procMan.writeGameInput ( remotePlayer, netMan.getInput ( remotePlayer ) );
    }

    void netplayStateChanged ( NetplayState state )
    {
        ASSERT ( netMan.getState() != state );

        // Clear the last overlay message
        if ( !DllOverlayUi::isShowingMessage() )
            DllOverlayUi::disable();

#ifdef RELEASE
        // Leaving Initial or AutoCharaSelect
        if ( netMan.getState() == NetplayState::Initial || netMan.getState() == NetplayState::AutoCharaSelect )
        {
            SetForegroundWindow ( ( HWND ) DllHacks::windowHandle );
        }
#endif // RELEASE

        // Leaving Skippable
        if ( netMan.getState() == NetplayState::Skippable )
        {
            roundOverTimer = -1;
            lazyDisconnect = false;
        }

        // Entering InGame
        if ( state == NetplayState::InGame )
        {
            if ( netMan.config.rollback )
                procMan.allocateStates();
        }

        // Leaving InGame
        if ( netMan.getState() == NetplayState::InGame )
        {
            if ( netMan.config.rollback )
                procMan.deallocateStates();
        }

        // Entering CharaSelect OR entering InGame
        if ( !clientMode.isOffline() && ( state == NetplayState::CharaSelect || state == NetplayState::InGame ) )
        {
            shouldSyncRngState = true;
        }

        // Entering RetryMenu
        if ( state == NetplayState::RetryMenu )
        {
            // Lazy disconnect now during netplay
            lazyDisconnect = clientMode.isNetplay();

            // Reset retry menu index flag
            localRetryMenuIndexSent = false;
        }
        else if ( lazyDisconnect )
        {
            lazyDisconnect = false;

            // If not entering RetryMenu and we're already disconnected...
            if ( !dataSocket || !dataSocket->isConnected() )
            {
                delayedStop ( "Disconnected!" );
                return;
            }
        }

        netMan.setState ( state );

        if ( dataSocket && dataSocket->isConnected() )
            dataSocket->send ( new TransitionIndex ( netMan.getIndex() ) );
    }

    void gameModeChanged ( uint32_t previous, uint32_t current )
    {
        if ( current == 0
                || current == CC_GAME_MODE_STARTUP
                || current == CC_GAME_MODE_OPENING
                || current == CC_GAME_MODE_TITLE
                || current == CC_GAME_MODE_MAIN
                || current == CC_GAME_MODE_LOADING_DEMO
                || ( previous == CC_GAME_MODE_LOADING_DEMO && current == CC_GAME_MODE_IN_GAME )
                || current == CC_GAME_MODE_HIGH_SCORES )
        {
            ASSERT ( netMan.getState() == NetplayState::PreInitial || netMan.getState() == NetplayState::Initial );
            return;
        }

        if ( netMan.getState() == NetplayState::Initial
#ifdef RELEASE
                && netMan.config.mode.isSpectate()
#else
                && ( netMan.config.mode.isSpectate() || replayInputs )
#endif // RELEASE
                && netMan.initial.netplayState > NetplayState::CharaSelect )
        {
            // Spectate mode needs to auto select characters if starting after CharaSelect
            netplayStateChanged ( NetplayState::AutoCharaSelect );
            return;
        }

        if ( current == CC_GAME_MODE_CHARA_SELECT )
        {
            netplayStateChanged ( NetplayState::CharaSelect );
            return;
        }

        if ( current == CC_GAME_MODE_LOADING )
        {
            netplayStateChanged ( NetplayState::Loading );
            return;
        }

        if ( current == CC_GAME_MODE_IN_GAME )
        {
            // Versus mode in-game starts with character intros, which is a skippable state
            if ( netMan.config.mode.isVersus() )
                netplayStateChanged ( NetplayState::Skippable );
            else
                netplayStateChanged ( NetplayState::InGame );
            return;
        }

        if ( current == CC_GAME_MODE_RETRY )
        {
            netplayStateChanged ( NetplayState::RetryMenu );
            return;
        }

        THROW_EXCEPTION ( "gameModeChanged(%u, %u)", ERROR_INVALID_GAME_MODE, previous, current );
    }

    void delayedStop ( const string& error )
    {
        if ( !error.empty() )
            procMan.ipcSend ( new ErrorMessage ( error ) );

        stopTimer.reset ( new Timer ( this ) );
        stopTimer->start ( DELAYED_STOP );
    }

    void startRoundOverCountDown()
    {
        ASSERT ( netMan.config.rollback > 0 );
        roundOverTimer = netMan.config.rollback + ROLLBACK_ROUND_OVER_DELAY;
    }

    void checkRoundOver()
    {
        if ( ! ( netMan.getState() == NetplayState::InGame && *CC_SKIPPABLE_FLAG_ADDR ) )
        {
            ASSERT ( netMan.config.rollback > 0 );
            roundOverTimer = -1;
            return;
        }

        roundOverTimer = -1;

        // Update NetplayState
        netplayStateChanged ( NetplayState::Skippable );
    }

    // ChangeMonitor callback
    void hasChanged ( Variable var, uint8_t previous, uint8_t current ) override
    {
        switch ( var.value )
        {
            case Variable::IntroState:
                if ( ! ( previous == 2 && current == 1 && netMan.getState() == NetplayState::Skippable ) )
                    break;

                // In-game happens when intro state is 1, ie when players can start moving
                LOG ( "[%s] %s: previous=%u; current=%u", netMan.getIndexedFrame(), var, previous, current );
                netplayStateChanged ( NetplayState::InGame );
                break;

            default:
                LOG ( "[%s] %s: previous=%u; current=%u", netMan.getIndexedFrame(), var, previous, current );
                break;
        }
    }

    void hasChanged ( Variable var, uint32_t previous, uint32_t current ) override
    {
        switch ( var.value )
        {
            case Variable::WorldTime:
                frameStep();
                break;

            case Variable::GameMode:
                LOG ( "[%s] %s: previous=%u; current=%u", netMan.getIndexedFrame(), var, previous, current );
                gameModeChanged ( previous, current );
                break;

            case Variable::SkippableFlag:
                if ( clientMode.isTraining()
                        || ! ( previous == 0 && current == 1 && netMan.isInGame() ) )
                    break;

                // When the SkippableFlag is set while InGame (not training mode), we are in a Skippable state
                LOG ( "[%s] %s: previous=%u; current=%u", netMan.getIndexedFrame(), var, previous, current );

                if ( netMan.config.rollback ) // Delay the check during rollback
                    startRoundOverCountDown();
                else
                    checkRoundOver();
                break;

            default:
                LOG ( "[%s] %s: previous=%u; current=%u", netMan.getIndexedFrame(), var, previous, current );
                break;
        }
    }

    // Socket callbacks
    void acceptEvent ( Socket *serverSocket ) override
    {
        LOG ( "acceptEvent ( %08x )", serverSocket );

        if ( serverSocket == serverCtrlSocket.get() )
        {
            LOG ( "serverCtrlSocket->accept ( this )" );

            SocketPtr newSocket = serverCtrlSocket->accept ( this );

            LOG ( "newSocket=%08x", newSocket.get() );

            ASSERT ( newSocket != 0 );
            ASSERT ( newSocket->isConnected() == true );

            IpAddrPort redirectAddr;

            if ( SHOULD_REDIRECT_SPECTATORS )
                redirectAddr = getRandomRedirectAddress();

            if ( redirectAddr.empty() )
            {
                newSocket->send ( new VersionConfig ( clientMode ) );
            }
            else
            {
                redirectedSockets.insert ( newSocket.get() );
                newSocket->send ( redirectAddr );
            }

            pushPendingSocket ( this, newSocket );
        }
        else if ( serverSocket == serverDataSocket.get() && !dataSocket )
        {
            LOG ( "serverDataSocket->accept ( this )" );

            dataSocket = serverDataSocket->accept ( this );

            LOG ( "dataSocket=%08x", dataSocket.get() );

            ASSERT ( dataSocket != 0 );
            ASSERT ( dataSocket->isConnected() == true );

            netplayStateChanged ( NetplayState::Initial );

            initialTimer.reset();
        }
        else
        {
            LOG ( "Unexpected acceptEvent from serverSocket=%08x", serverSocket );
            serverSocket->accept ( 0 ).reset();
            return;
        }
    }

    void connectEvent ( Socket *socket ) override
    {
        LOG ( "connectEvent ( %08x )", socket );

        ASSERT ( dataSocket.get() != 0 );
        ASSERT ( dataSocket->isConnected() == true );

        dataSocket->send ( serverCtrlSocket->address );

        netplayStateChanged ( NetplayState::Initial );

        initialTimer.reset();
    }

    void disconnectEvent ( Socket *socket ) override
    {
        LOG ( "disconnectEvent ( %08x )", socket );

        if ( socket == dataSocket.get() )
        {
            if ( netMan.getState() == NetplayState::PreInitial )
            {
                dataSocket = SmartSocket::connectUDP ( this, address );
                LOG ( "dataSocket=%08x", dataSocket.get() );
                return;
            }

            if ( lazyDisconnect )
                return;

            delayedStop ( "Disconnected!" );
            return;
        }

        redirectedSockets.erase ( socket );
        popPendingSocket ( socket );
        popSpectator ( socket );
    }

    void readEvent ( Socket *socket, const MsgPtr& msg, const IpAddrPort& address ) override
    {
        LOG ( "readEvent ( %08x, %s, %s )", socket, msg, address );

        if ( !msg.get() )
            return;

        if ( redirectedSockets.find ( socket ) != redirectedSockets.end() )
            return;

        switch ( msg->getMsgType() )
        {
            case MsgType::VersionConfig:
            {
                const Version RemoteVersion = msg->getAs<VersionConfig>().version;

                if ( !LocalVersion.similar ( RemoteVersion, 1 + options[Options::StrictVersion] ) )
                {
                    string local = LocalVersion.code;
                    string remote = RemoteVersion.code;

                    if ( options[Options::StrictVersion] >= 2 )
                    {
                        local += " " + LocalVersion.revision;
                        remote += " " + RemoteVersion.revision;
                    }

                    if ( options[Options::StrictVersion] >= 3 )
                    {
                        local += " " + LocalVersion.buildTime;
                        remote += " " + RemoteVersion.buildTime;
                    }

                    LOG ( "Incompatible versions:\nLocal version: %s\nRemote version: %s", local, remote );

                    socket->disconnect();
                    return;
                }

                socket->send ( new SpectateConfig ( netMan.config, netMan.getState().value ) );
                return;
            }

            case MsgType::ConfirmConfig:
                // Wait for IpAddrPort before actually adding this new spectator
                return;

            case MsgType::IpAddrPort:
                if ( socket == dataSocket.get() || !isPendingSocket ( socket ) )
                    break;

                pushSpectator ( socket, { socket->address.addr, msg->getAs<IpAddrPort>().port } );
                return;

            case MsgType::RngState:
                netMan.setRngState ( msg->getAs<RngState>() );
                return;

#ifndef RELEASE
            case MsgType::SyncHash:
                remoteSync.push_back ( msg );
                return;
#endif // RELEASE

            default:
                break;
        }

        switch ( clientMode.value )
        {
            case ClientMode::Host:
                if ( msg->getMsgType() == MsgType::IpAddrPort && socket == dataSocket.get() )
                {
                    clientServerAddr = msg->getAs<IpAddrPort>();
                    clientServerAddr.addr = dataSocket->address.addr;
                    clientServerAddr.invalidate();
                    return;
                }

            case ClientMode::Client:
                switch ( msg->getMsgType() )
                {
                    case MsgType::PlayerInputs:
                        netMan.setInputs ( remotePlayer, msg->getAs<PlayerInputs>() );
                        return;

                    case MsgType::MenuIndex:
                        netMan.setRemoteRetryMenuIndex ( msg->getAs<MenuIndex>().menuIndex );
                        return;

                    case MsgType::ChangeConfig:
                        // Only use the ChangeConfig if it is for a later frame than the current ChangeConfig.
                        // If for the same frame, then the host's ChangeConfig always takes priority.
                        if ( ( msg->getAs<ChangeConfig>().indexedFrame.value > changeConfig.indexedFrame.value )
                                || ( msg->getAs<ChangeConfig>().indexedFrame.value == changeConfig.indexedFrame.value
                                     && clientMode.isClient() ) )
                        {
                            shouldChangeDelayRollback = true;
                            changeConfig = msg->getAs<ChangeConfig>();
                        }
                        return;

                    case MsgType::TransitionIndex:
                        netMan.setRemoteIndex ( msg->getAs<TransitionIndex>().index );
                        return;

                    case MsgType::ErrorMessage:
                        if ( lazyDisconnect )
                            return;

                        delayedStop ( msg->getAs<ErrorMessage>().error );
                        return;

                    default:
                        break;
                }
                break;

            case ClientMode::SpectateNetplay:
            case ClientMode::SpectateBroadcast:
                switch ( msg->getMsgType() )
                {
                    case MsgType::InitialGameState:
                        netMan.initial = msg->getAs<InitialGameState>();

                        if ( netMan.initial.chara[0] == UNKNOWN_POSITION )
                            THROW_EXCEPTION ( "chara[0]=Unknown", ERROR_INVALID_HOST_CONFIG );

                        if ( netMan.initial.chara[1] == UNKNOWN_POSITION )
                            THROW_EXCEPTION ( "chara[1]=Unknown", ERROR_INVALID_HOST_CONFIG );

                        if ( netMan.initial.moon[0] == UNKNOWN_POSITION )
                            THROW_EXCEPTION ( "moon[0]=Unknown", ERROR_INVALID_HOST_CONFIG );

                        if ( netMan.initial.moon[1] == UNKNOWN_POSITION )
                            THROW_EXCEPTION ( "moon[1]=Unknown", ERROR_INVALID_HOST_CONFIG );

                        LOG ( "InitialGameState: %s; indexedFrame=[%s]; stage=%u; isTraining=%u; %s vs %s",
                              NetplayState ( ( NetplayState::Enum ) netMan.initial.netplayState ),
                              netMan.initial.indexedFrame, netMan.initial.stage, netMan.initial.isTraining,
                              netMan.initial.formatCharaName ( 1, getFullCharaName ),
                              netMan.initial.formatCharaName ( 2, getFullCharaName ) );

                        netplayStateChanged ( NetplayState::Initial );
                        return;

                    case MsgType::BothInputs:
                        netMan.setBothInputs ( msg->getAs<BothInputs>() );
                        return;

                    case MsgType::MenuIndex:
                        netMan.setRetryMenuIndex ( msg->getAs<MenuIndex>().index, msg->getAs<MenuIndex>().menuIndex );
                        return;

                    case MsgType::ErrorMessage:
                        delayedStop ( msg->getAs<ErrorMessage>().error );
                        return;

                    default:
                        break;
                }
                break;

            default:
                break;
        }

        LOG ( "Unexpected '%s' from socket=%08x", msg, socket );
    }

    // ProcessManager callbacks
    void ipcConnectEvent() override
    {
    }

    void ipcDisconnectEvent() override
    {
        appState = AppState::Stopping;
        EventManager::get().stop();
    }

    void ipcReadEvent ( const MsgPtr& msg ) override
    {
        if ( !msg.get() )
            return;

        switch ( msg->getMsgType() )
        {
            case MsgType::OptionsMessage:
                options = msg->getAs<OptionsMessage>();

                Logger::get().sessionId = options.arg ( Options::SessionId );
                Logger::get().initialize ( options.arg ( Options::AppDir ) + LOG_FILE );
                Logger::get().logVersion();

                LOG ( "SessionId '%s'", Logger::get().sessionId );

                syncLog.sessionId = options.arg ( Options::SessionId );
                syncLog.initialize ( options.arg ( Options::AppDir ) + SYNC_LOG_FILE, 0 );
                syncLog.logVersion();

#ifndef RELEASE
                if ( options[Options::Replay] )
                {
                    LOG ( "Replay: '%s'", options.arg ( Options::Replay ) );

                    const vector<string> args = split ( options.arg ( Options::Replay ), "," );

                    ASSERT ( args.empty() == false );

                    const string replayFile = options.arg ( Options::AppDir ) + args[0];
                    const bool real = find ( args.begin(), args.end(), "real" ) != args.end();

                    auto it = find ( args.begin(), args.end(), "start" );
                    if ( it != args.end() )
                        ++it;
                    if ( it != args.end() && ( args.end() - it ) >= 7 ) // TODO only need one arg
                    {
                        netMan.initial.indexedFrame.parts.frame = 0;
                        netMan.initial.netplayState = 0xFF;
                        netMan.initial.stage = 1;

                        netMan.initial.indexedFrame.parts.index = lexical_cast<int> ( *it++ );

                        // TODO fetch these args from the replay file
                        netMan.initial.chara[0]                 = lexical_cast<int> ( *it++ );
                        netMan.initial.moon[0]                  = lexical_cast<int> ( *it++ );
                        netMan.initial.color[0]                 = lexical_cast<int> ( *it++ );
                        netMan.initial.chara[1]                 = lexical_cast<int> ( *it++ );
                        netMan.initial.moon[1]                  = lexical_cast<int> ( *it++ );
                        netMan.initial.color[1]                 = lexical_cast<int> ( *it++ );
                    }

                    it = find ( args.begin(), args.end(), "stop" );
                    if ( it != args.end() )
                        ++it;
                    if ( it != args.end() && ( args.end() - it ) >= 2 )
                    {
                        replayStop.parts.index = lexical_cast<uint32_t> ( *it++ );
                        replayStop.parts.frame = lexical_cast<uint32_t> ( *it++ );
                    }

                    const bool good = repMan.load ( replayFile, real );

                    ASSERT ( good == true );

                    replayInputs = true;
                }
                else
                {
                    randomInputs = options[Options::SyncTest];
                }
#endif // RELEASE
                break;

            case MsgType::ControllerMappings:
                KeyboardState::clear();
                ControllerManager::get().owner = this;
                ControllerManager::get().getKeyboard()->setMappings ( ProcessManager::fetchKeyboardConfig() );
                ControllerManager::get().setMappings ( msg->getAs<ControllerMappings>() );
                ControllerManager::get().check();
                allControllers = ControllerManager::get().getControllers();
                break;

            case MsgType::ClientMode:
                if ( clientMode != ClientMode::Unknown )
                    break;

                clientMode = msg->getAs<ClientMode>();
                clientMode.flags |= ClientMode::GameStarted;

                if ( clientMode.isTraining() )
                    WRITE_ASM_HACK ( AsmHacks::forceGotoTraining );
                else if ( clientMode.isVersusCpu() )
                    WRITE_ASM_HACK ( AsmHacks::forceGotoVersusCpu );
                else
                    WRITE_ASM_HACK ( AsmHacks::forceGotoVersus );

                isSinglePlayer = clientMode.isSinglePlayer();

                LOG ( "%s: flags={ %s }", clientMode, clientMode.flagString() );
                break;

            case MsgType::IpAddrPort:
                if ( !address.empty() )
                    break;

                address = msg->getAs<IpAddrPort>();
                LOG ( "address='%s'", address );
                break;

            case MsgType::SpectateConfig:
                ASSERT ( clientMode.isSpectate() == true );

                netMan.config.mode       = clientMode;
                netMan.config.mode.flags |= msg->getAs<SpectateConfig>().mode.flags;
                netMan.config.sessionId  = Logger::get().sessionId;
                netMan.config.delay      = msg->getAs<SpectateConfig>().delay;
                netMan.config.rollback   = msg->getAs<SpectateConfig>().rollback;
                netMan.config.winCount   = msg->getAs<SpectateConfig>().winCount;
                netMan.config.hostPlayer = msg->getAs<SpectateConfig>().hostPlayer;
                netMan.config.names      = msg->getAs<SpectateConfig>().names;
                netMan.config.sessionId  = msg->getAs<SpectateConfig>().sessionId;

                if ( netMan.config.delay == 0xFF )
                    THROW_EXCEPTION ( "delay=%u", ERROR_INVALID_HOST_CONFIG, netMan.config.delay );

                netMan.initial = msg->getAs<SpectateConfig>().initial;

                if ( netMan.initial.netplayState == NetplayState::Unknown )
                    THROW_EXCEPTION ( "netplayState=NetplayState::Unknown", ERROR_INVALID_HOST_CONFIG );

                if ( netMan.initial.chara[0] == UNKNOWN_POSITION )
                    THROW_EXCEPTION ( "chara[0]=Unknown", ERROR_INVALID_HOST_CONFIG );

                if ( netMan.initial.chara[1] == UNKNOWN_POSITION )
                    THROW_EXCEPTION ( "chara[1]=Unknown", ERROR_INVALID_HOST_CONFIG );

                if ( netMan.initial.moon[0] == UNKNOWN_POSITION )
                    THROW_EXCEPTION ( "moon[0]=Unknown", ERROR_INVALID_HOST_CONFIG );

                if ( netMan.initial.moon[1] == UNKNOWN_POSITION )
                    THROW_EXCEPTION ( "moon[1]=Unknown", ERROR_INVALID_HOST_CONFIG );

                LOG ( "SpectateConfig: %s; flags={ %s }; delay=%d; rollback=%d; winCount=%d; hostPlayer=%u; "
                      "names={ '%s', '%s' }", netMan.config.mode, netMan.config.mode.flagString(), netMan.config.delay,
                      netMan.config.rollback, netMan.config.winCount, netMan.config.hostPlayer,
                      netMan.config.names[0], netMan.config.names[1] );

                LOG ( "InitialGameState: %s; stage=%u; isTraining=%u; %s vs %s",
                      NetplayState ( ( NetplayState::Enum ) netMan.initial.netplayState ),
                      netMan.initial.stage, netMan.initial.isTraining,
                      msg->getAs<SpectateConfig>().formatPlayer ( 1, getFullCharaName ),
                      msg->getAs<SpectateConfig>().formatPlayer ( 2, getFullCharaName ) );

                serverCtrlSocket = SmartSocket::listenTCP ( this, 0 );
                LOG ( "serverCtrlSocket=%08x", serverCtrlSocket.get() );

                procMan.ipcSend ( serverCtrlSocket->address );

                *CC_DAMAGE_LEVEL_ADDR = 2;
                *CC_TIMER_SPEED_ADDR = 2;
                *CC_WIN_COUNT_VS_ADDR = ( uint32_t ) ( netMan.config.winCount ? netMan.config.winCount : 2 );

                // *CC_WIN_COUNT_VS_ADDR = 1;
                // *CC_DAMAGE_LEVEL_ADDR = 4;

                // Wait for final InitialGameState message before going to NetplayState::Initial
                break;

            case MsgType::NetplayConfig:
                if ( netMan.config.delay != 0xFF )
                    break;

                netMan.config = msg->getAs<NetplayConfig>();
                netMan.config.mode = clientMode;
                netMan.config.sessionId = Logger::get().sessionId;

                if ( netMan.config.delay == 0xFF )
                    THROW_EXCEPTION ( "delay=%u", ERROR_INVALID_HOST_CONFIG, netMan.config.delay );

                if ( clientMode.isNetplay() )
                {
                    if ( netMan.config.hostPlayer != 1 && netMan.config.hostPlayer != 2 )
                        THROW_EXCEPTION ( "hostPlayer=%u", ERROR_INVALID_HOST_CONFIG, netMan.config.hostPlayer );

                    // Determine the player numbers
                    if ( clientMode.isHost() )
                    {
                        localPlayer = netMan.config.hostPlayer;
                        remotePlayer = ( 3 - netMan.config.hostPlayer );
                    }
                    else
                    {
                        remotePlayer = netMan.config.hostPlayer;
                        localPlayer = ( 3 - netMan.config.hostPlayer );
                    }

                    netMan.setRemotePlayer ( remotePlayer );

                    if ( clientMode.isHost() )
                    {
                        serverCtrlSocket = SmartSocket::listenTCP ( this, address.port );
                        LOG ( "serverCtrlSocket=%08x", serverCtrlSocket.get() );

                        serverDataSocket = SmartSocket::listenUDP ( this, address.port );
                        LOG ( "serverDataSocket=%08x", serverDataSocket.get() );
                    }
                    else if ( clientMode.isClient() )
                    {
                        serverCtrlSocket = SmartSocket::listenTCP ( this, 0 );
                        LOG ( "serverCtrlSocket=%08x", serverCtrlSocket.get() );

                        dataSocket = SmartSocket::connectUDP ( this, address, clientMode.isUdpTunnel() );
                        LOG ( "dataSocket=%08x", dataSocket.get() );
                    }

                    initialTimer.reset ( new Timer ( this ) );
                    initialTimer->start ( INITIAL_CONNECT_TIMEOUT );

                    // Wait for dataSocket to be connected before changing to NetplayState::Initial
                }
                else if ( clientMode.isBroadcast() )
                {
                    ASSERT ( netMan.config.mode.isBroadcast() == true );

                    LOG ( "NetplayConfig: broadcastPort=%u", netMan.config.broadcastPort );

                    serverCtrlSocket = SmartSocket::listenTCP ( this, netMan.config.broadcastPort );
                    LOG ( "serverCtrlSocket=%08x", serverCtrlSocket.get() );

                    // Update the broadcast port and send over IPC
                    netMan.config.broadcastPort = serverCtrlSocket->address.port;
                    netMan.config.invalidate();

                    procMan.ipcSend ( netMan.config );

                    netplayStateChanged ( NetplayState::Initial );
                }
                else if ( clientMode.isOffline() )
                {
                    ASSERT ( netMan.config.hostPlayer == 1 || netMan.config.hostPlayer == 2 );

                    localPlayer = netMan.config.hostPlayer;
                    remotePlayer = ( 3 - netMan.config.hostPlayer );

                    netMan.setRemotePlayer ( remotePlayer );

                    netplayStateChanged ( NetplayState::Initial );
                }

                *CC_DAMAGE_LEVEL_ADDR = 2;
                *CC_TIMER_SPEED_ADDR = 2;
                *CC_WIN_COUNT_VS_ADDR = ( uint32_t ) ( netMan.config.winCount ? netMan.config.winCount : 2 );

                // *CC_WIN_COUNT_VS_ADDR = 1;
                // *CC_DAMAGE_LEVEL_ADDR = 4;

                // Rollback specific game hacks
                if ( netMan.config.rollback )
                {
                    // Manually control intro state
                    WRITE_ASM_HACK ( AsmHacks::hijackIntroState );

                    // Disable auto replay save
                    *CC_AUTO_REPLAY_SAVE_ADDR = 0;
                }

                LOG ( "SessionId '%s'", netMan.config.sessionId );

                LOG ( "NetplayConfig: %s; flags={ %s }; delay=%d; rollback=%d; rollbackDelay=%d; winCount=%d; "
                      "hostPlayer=%d; localPlayer=%d; remotePlayer=%d; names={ '%s', '%s' }",
                      netMan.config.mode, netMan.config.mode.flagString(), netMan.config.delay, netMan.config.rollback,
                      netMan.config.rollbackDelay, netMan.config.winCount, netMan.config.hostPlayer,
                      localPlayer, remotePlayer, netMan.config.names[0], netMan.config.names[1] );
                break;

            default:
                if ( clientMode.isSpectate() )
                {
                    readEvent ( 0, msg, NullAddress );
                    break;
                }

                LOG ( "Unexpected '%s'", msg );
                break;
        }
    }

    // Timer callback
    void timerExpired ( Timer *timer ) override
    {
        if ( timer == resendTimer.get() )
        {
            dataSocket->send ( netMan.getInputs ( localPlayer ) );
            resendTimer->start ( RESEND_INPUTS_INTERVAL );

            ++waitInputsTimer;

            if ( waitInputsTimer > ( MAX_WAIT_INPUTS_INTERVAL / RESEND_INPUTS_INTERVAL ) )
                delayedStop ( "Timed out!" );
        }
        else if ( timer == initialTimer.get() )
        {
            delayedStop ( "Disconnected!" );
            initialTimer.reset();
        }
        else if ( timer == stopTimer.get() )
        {
            appState = AppState::Stopping;
            EventManager::get().stop();
        }
        else
        {
            SpectatorManager::timerExpired ( timer );
        }
    }

    // DLL callback
    void callback()
    {
        // Check if the game is being closed
        if ( ! ( * CC_ALIVE_FLAG_ADDR ) )
        {
            // Disconnect the main data socket if netplay
            if ( clientMode.isNetplay() && dataSocket )
                dataSocket->disconnect();

            // Disconnect all other sockets
            if ( ctrlSocket )
                ctrlSocket->disconnect();

            if ( serverCtrlSocket )
                serverCtrlSocket->disconnect();

            appState = AppState::Stopping;
            EventManager::get().stop();
        }

        // Don't poll unless we're in the correct state
        if ( appState != AppState::Polling )
            return;

        // Check if the world timer changed, this calls hasChanged if changed, which calls frameStep
        worldTimerMoniter.check();
    }

    // Constructor
    DllMain()
        : SpectatorManager ( &netMan, &procMan )
        , worldTimerMoniter ( this, Variable::WorldTime, *CC_WORLD_TIMER_ADDR )
    {
        // Timer and controller initialization is not done here because of threading issues

        procMan.connectPipe();

        netplayStateChanged ( NetplayState::PreInitial );

        ChangeMonitor::get().addRef ( this, Variable ( Variable::GameMode ), *CC_GAME_MODE_ADDR );
        ChangeMonitor::get().addRef ( this, Variable ( Variable::SkippableFlag ), *CC_SKIPPABLE_FLAG_ADDR );
        ChangeMonitor::get().addRef ( this, Variable ( Variable::IntroState ), *CC_INTRO_STATE_ADDR );

#ifndef RELEASE
        ChangeMonitor::get().addRef ( this, Variable ( Variable::MenuConfirmState ), AsmHacks::menuConfirmState );
        ChangeMonitor::get().addRef ( this, Variable ( Variable::CurrentMenuIndex ), AsmHacks::currentMenuIndex );
        // ChangeMonitor::get().addRef ( this, Variable ( Variable::GameStateCounter ), *CC_MENU_STATE_COUNTER_ADDR );
        // ChangeMonitor::get().addPtrToRef ( this, Variable ( Variable::AutoReplaySave ),
        //                                    const_cast<const uint32_t *&> ( AsmHacks::autoReplaySaveStatePtr ), 0u );
#endif // RELEASE
    }

    // Destructor
    ~DllMain()
    {
        KeyboardManager::get().unhook();

        syncLog.deinitialize();

        procMan.disconnectPipe();

        ControllerManager::get().owner = 0;

        // Timer and controller deinitialization is not done here because of threading issues
    }

private:

    void saveMappings ( const Controller *controller ) const override
    {
        if ( !controller )
            return;

        const string file = options.arg ( Options::AppDir ) + FOLDER + controller->getName() + MAPPINGS_EXT;

        LOG ( "Saving: %s", file );

        if ( controller->saveMappings ( file ) )
            return;

        LOG ( "Failed to save: %s", file );
    }

    const IpAddrPort& getRandomRedirectAddress() const
    {
        size_t r = rand() % ( 1 + numSpectators() );

        if ( r == 0 && !clientServerAddr.empty() )
            return clientServerAddr;
        else
            return getRandomSpectatorAddress();
    }
};


static void initializeDllMain()
{
    main.reset ( new DllMain() );
}

static void deinitialize()
{
    LOCK ( deinitMutex );

    if ( appState == AppState::Deinitialized )
        return;

    main.reset();

    EventManager::get().release();
    TimerManager::get().deinitialize();
    SocketManager::get().deinitialize();
    // Joystick must be deinitialized on the same thread it was initialized, ie not here
    Logger::get().deinitialize();

    DllHacks::deinitialize();

    appState = AppState::Deinitialized;
}

extern "C" BOOL APIENTRY DllMain ( HMODULE, DWORD reason, LPVOID )
{
    switch ( reason )
    {
        case DLL_PROCESS_ATTACH:
        {
            char buffer[4096];
            string gameDir;

            if ( GetModuleFileName ( GetModuleHandle ( 0 ), buffer, sizeof ( buffer ) ) )
            {
                gameDir = buffer;
                gameDir = gameDir.substr ( 0, gameDir.find_last_of ( "/\\" ) );

                replace ( gameDir.begin(), gameDir.end(), '/', '\\' );

                if ( !gameDir.empty() && gameDir.back() != '\\' )
                    gameDir += '\\';
            }

            ProcessManager::gameDir = gameDir;

            srand ( time ( 0 ) );

            Logger::get().initialize ( gameDir + LOG_FILE );
            Logger::get().logVersion();
            LOG ( "DLL_PROCESS_ATTACH" );

            // We want the DLL to be able to rebind any previously bound ports
            Socket::forceReusePort ( true );

            try
            {
                // It is safe to initialize sockets here
                SocketManager::get().initialize();
                DllHacks::initializePreLoad();
                initializeDllMain();
            }
            catch ( const Exception& exc )
            {
                exit ( -1 );
            }
#ifdef NDEBUG
            catch ( const std::exception& exc )
            {
                exit ( -1 );
            }
            catch ( ... )
            {
                exit ( -1 );
            }
#endif // NDEBUG
            break;
        }

        case DLL_PROCESS_DETACH:
            LOG ( "DLL_PROCESS_DETACH" );
            appState = AppState::Stopping;
            EventManager::get().release();
            exit ( 0 );
            break;
    }

    return TRUE;
}


static void stopDllMain ( const string& error )
{
    if ( main )
    {
        main->delayedStop ( error );
    }
    else
    {
        appState = AppState::Stopping;
        EventManager::get().stop();
    }
}

namespace AsmHacks
{

extern "C" void callback()
{
    if ( appState == AppState::Deinitialized )
        return;

    try
    {
        if ( appState == AppState::Uninitialized )
        {
            DllHacks::initializePostLoad();
            KeyboardState::windowHandle = DllHacks::windowHandle;

            // Joystick and timer must be initialized in the main thread
            TimerManager::get().initialize();
            ControllerManager::get().initialize ( 0 );
            ControllerManager::get().windowHandle = DllHacks::windowHandle;

            // Start polling now
            EventManager::get().startPolling();
            appState = AppState::Polling;
        }

        ASSERT ( main.get() != 0 );

        main->callback();
    }
    catch ( const Exception& exc )
    {
        LOG ( "Stopping due to exception: %s", exc );
        stopDllMain ( exc.user );
    }
#ifdef NDEBUG
    catch ( const std::exception& exc )
    {
        LOG ( "Stopping due to std::exception: %s", exc.what() );
        stopDllMain ( string ( "Error: " ) + exc.what() );
    }
    catch ( ... )
    {
        LOG ( "Stopping due to unknown exception!" );
        stopDllMain ( "Unknown error!" );
    }
#endif // NDEBUG

    if ( appState == AppState::Stopping )
    {
        LOG ( "Exiting" );

        // Joystick must be deinitialized on the main thread it was initialized
        ControllerManager::get().deinitialize();
        deinitialize();
        exit ( 0 );
    }
}

} // namespace AsmHacks
