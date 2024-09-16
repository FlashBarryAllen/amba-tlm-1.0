#ifndef ARM_AXI4_MULTI_SOCKET_H
#define ARM_AXI4_MULTI_SOCKET_H

#include "arm_axi4_payload.h"
#include "arm_axi4_phase.h"
#include "arm_tlm_multi_socket.h"

namespace ARM
{
namespace AXI4
{

/**
 * Payload and Phase grouped for use with socket templates in a similar
 * way to tlm::tlm_base_protocol_types.
 */
class MyProtocolType
{
public:
    typedef Payload tlm_payload_type;
    typedef Phase tlm_phase_type;
};

/** ARM::TLM::MasterMultiSockets specialised for AXI4 payloads/phases. */
template <typename Module, typename Types = MyProtocolType>
class MasterMultiSockets : public ARM::TLM::MasterMultiSockets <Module, Types>
{
private:
    typedef typename ARM::TLM::MasterMultiSockets<Module, Types> BaseType;

public:
    MasterMultiSockets(const char* name_, Module& t,
        typename BaseType::NBFunc bw,
        TLM::Protocol protocol_, unsigned width_) :
        TLM::MasterMultiSockets <Module, Types>(name_, t, bw, protocol_, width_)
    {}
};

/** ARM::TLM::SlaveMultiSockets specialised for AXI4 payloads/phases. */
template <typename Module, typename Types = MyProtocolType>
class SlaveMultiSockets : public ARM::TLM::SlaveMultiSockets <Module, Types>
{
private:
    typedef typename ARM::TLM::SlaveMultiSockets<Module, Types> BaseType;

public:
    SlaveMultiSockets(const char* name_, Module& t,
        typename BaseType::NBFunc fw,
        TLM::Protocol protocol_, unsigned width_,
        typename BaseType::DebugFunc dbg = NULL) :
        TLM::SlaveMultiSockets <Module, Types>(name_, t, fw,
            protocol_, width_, dbg)
    {}
};

}
}

#endif // ARM_AXI4_SOCKET_H
