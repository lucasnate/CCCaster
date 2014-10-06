#pragma once

#include "Constants.h"
#include "Protocol.h"
#include "Logger.h"
#include "Statistics.h"

#include <cereal/types/array.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/unordered_map.hpp>

#include <array>


struct EndOfMessages : public SerializableSequence { EMPTY_MESSAGE_BOILERPLATE ( EndOfMessages ) };
struct CharaSelectLoaded : public SerializableSequence { EMPTY_MESSAGE_BOILERPLATE ( CharaSelectLoaded ) };


struct ErrorMessage : public SerializableSequence
{
    std::string error;

    ErrorMessage ( const std::string& error ) : error ( error ) {}

    PROTOCOL_MESSAGE_BOILERPLATE ( ErrorMessage, error )
};


struct ClientType : public SerializableSequence
{
    ENUM_MESSAGE_BOILERPLATE ( ClientType, Host, Client, Broadcast, Offline )
};


struct InitialConfig : public SerializableSequence
{
    std::string remoteName;
    uint8_t training = 0;
    Statistics stats;
    uint8_t packetLoss = 0;

    PROTOCOL_MESSAGE_BOILERPLATE ( InitialConfig, remoteName, training, stats, packetLoss )
};


struct NetplayConfig : public SerializableSequence
{
    uint8_t delay = 0xFF, rollback = 0;
    uint8_t training = 0;
    uint8_t hostPlayer = 0;
    uint16_t broadcastPort = 0;

    uint8_t getOffset() const
    {
        if ( delay < rollback )
            return 0;
        else
            return delay - rollback;
    }

    PROTOCOL_MESSAGE_BOILERPLATE ( NetplayConfig, delay, rollback, training, hostPlayer, broadcastPort )
};


struct RngState : public SerializableSequence
{
    uint32_t rngState0, rngState1, rngState2;
    std::array<char, CC_RNGSTATE3_SIZE> rngState3;

    std::string dump() const
    {
        return toBase64 ( &rngState0, sizeof ( rngState0 ) )
               + " " + toBase64 ( &rngState1, sizeof ( rngState1 ) )
               + " " + toBase64 ( &rngState2, sizeof ( rngState2 ) )
               + " " + toBase64 ( &rngState3[0], rngState3.size() );
    }

    PROTOCOL_MESSAGE_BOILERPLATE ( RngState, rngState0, rngState1, rngState2, rngState3 );
};


struct PerGameData : public SerializableSequence
{
    uint32_t startIndex = 0;
    std::array<uint8_t, 2> chara, color, moon;

    // Mapping: index -> RngState
    std::unordered_map<uint32_t, RngState> rngStates;

    // Mapping: index -> player -> frame -> input
    std::vector<std::array<std::vector<uint32_t>, 2>> inputs;

    PerGameData ( uint32_t startIndex ) : startIndex ( startIndex ) {}

    PROTOCOL_MESSAGE_BOILERPLATE ( PerGameData, startIndex, chara, color, moon, rngStates, inputs );
};


struct BaseInputs
{
    IndexedFrame indexedFrame = { { 0, 0 } };

    uint32_t getIndex() const { return indexedFrame.parts.index; }
    uint32_t getFrame() const { return indexedFrame.parts.frame; }
    IndexedFrame getStartIndexedFrame() const { return { getIndex(), getStartFrame() }; }

    uint32_t getStartFrame() const
    {
        return ( indexedFrame.parts.frame + 1 < NUM_INPUTS ) ? 0 : indexedFrame.parts.frame + 1 - NUM_INPUTS;
    }

    uint32_t getEndFrame() const { return indexedFrame.parts.frame + 1; }

    size_t size() const { return getEndFrame() - getStartFrame(); }
};


struct PlayerInputs : public SerializableMessage, public BaseInputs
{
    // Represents the input range [frame - NUM_INPUTS + 1, frame + 1)
    std::array<uint16_t, NUM_INPUTS> inputs;

    PlayerInputs ( IndexedFrame indexedFrame ) { this->indexedFrame = indexedFrame; }

    PROTOCOL_MESSAGE_BOILERPLATE ( PlayerInputs, indexedFrame.value, inputs )
};


struct BothInputs : public SerializableMessage, public BaseInputs
{
    // Represents the input range [frame - NUM_INPUTS + 1, frame + 1)
    std::array<std::array<uint16_t, NUM_INPUTS>, 2> inputs;

    BothInputs ( IndexedFrame indexedFrame ) { this->indexedFrame = indexedFrame; }

    PROTOCOL_MESSAGE_BOILERPLATE ( BothInputs, indexedFrame.value, inputs )
};
