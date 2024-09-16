#ifndef ARM_TLM_MULTI_SOCKET_H
#define ARM_TLM_MULTI_SOCKET_H

#include <tlm.h>
#include <sstream>

#include "arm_tlm_protocol.h"
#include "utilities.h"
#include "tlm_utils/multi_passthrough_initiator_socket.h"
#include "tlm_utils/multi_passthrough_target_socket.h"

namespace ARM
{
namespace TLM
{

/* we must decleare BaseMasterSocket here, or we will get bug */
template <typename Types>
class BaseMasterMultiSockets;

/* Base slave socket implementing protocol/width checking. */
template <typename Types>
class BaseSlaveMultiSockets : public tlm_utils::multi_passthrough_target_socket<Types>
{
public:
    /* Protocol to test against other sockets when binding. */
    const Protocol protocol;

    /** Port data width to test against other sockets when binding. */
    const unsigned port_width;

public:
    BaseSlaveMultiSockets(const char* name_,
        Protocol protocol_, unsigned port_width_) :
        tlm_utils::multi_passthrough_target_socket<Types>(name_),
        protocol(protocol_),
        port_width(port_width_)
    {}
};

/** Base master socket implementing protocol/width checking. */
template <typename Types>
class BaseMasterMultiSockets : public tlm_utils::multi_passthrough_initiator_socket<Types>
{
public:
    /* Protocol to test against other sockets when binding. */
    const Protocol protocol;

    /** Port data width to test against other sockets when binding. */
    const unsigned port_width;

public:
    BaseMasterMultiSockets(const char* name_,
        Protocol protocol_, unsigned port_width_) :
        tlm_utils::multi_passthrough_initiator_socket<Types>(name_),
        protocol(protocol_),
        port_width(port_width_)
    {}
};

/**
 * Simple slave socket allowing a Module class to implement communication
 * functions as member functions and register them with the socket in the
 * same way at tlm_utils::simple_target_socket.
 */
template <typename Module, typename Types>
class SlaveMultiSockets : public BaseSlaveMultiSockets<Types>
{
protected:
    typedef typename Types::tlm_payload_type PayloadType;
    typedef typename Types::tlm_phase_type PhaseType;

public:
    /** Non blocking transport function pointer. */
    typedef tlm::tlm_sync_enum (Module::* NBFunc)(int, PayloadType&, PhaseType&);

    /** Debug transport function pointer. */
    typedef unsigned (Module::* DebugFunc)(PayloadType&);

protected:
    /** Proxy object implementing the fw transport interface. */
    class Proxy : public tlm::tlm_fw_transport_if<Types>
    {
    public:
        /** Owner of the proxy. */
        const SlaveMultiSockets<Module, Types>& owner;

        /** Object and functions to which to defer interface calls. */
        Module& t;
        NBFunc fw;
        DebugFunc dbg;

        Proxy(const SlaveMultiSockets<Module, Types>& owner_,
            Module& t_, NBFunc fw_, DebugFunc dbg_) :
            owner(owner_), t(t_), fw(fw_), dbg(dbg_)
        {}

        tlm::tlm_sync_enum nb_transport_fw(int id, PayloadType& trans,
            PhaseType& phase, sc_core::sc_time& delay)
        {
            return (t.*fw)(trans, phase);
        }

        void b_transport(int id, PayloadType& trans, sc_core::sc_time& delay)
        {
            std::ostringstream message;
            message << owner.name() << ": b_transport not implemented";
            SC_REPORT_ERROR("/ARM/TLM/SimpleSlaveSocket",
                message.str().c_str());
        }

        unsigned transport_dbg(PayloadType& trans)
        {
            if (dbg)
                return (t.*dbg)(trans);
            else
                return 0;
        }

        bool get_direct_mem_ptr(PayloadType&, tlm::tlm_dmi&)
        {
            return false;
        }
    };

    Proxy proxy;

public:
    SlaveMultiSockets(const char* name_, Module& t, NBFunc fw,
        Protocol protocol_, unsigned port_width_, DebugFunc dbg = NULL) :
        BaseSlaveMultiSockets<Types>(name_, protocol_, port_width_),
        proxy(*this, t, fw, dbg)
    {
        this->bind(proxy);
    }

    /** Convenience function for bw without time. */
    tlm::tlm_sync_enum nb_transport_bw(int id, PayloadType& trans, PhaseType& phase)
    {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        return (*this)->nb_transport_bw(trans, phase, delay);
    }

};

/**
 * Simple slave socket allowing a Module class to implement communication
 * functions as member functions and register them with the socket in the
 * same way at tlm_utils::simple_initiator_socket.
 */
template <typename Module, typename Types>
class MasterMultiSockets : public BaseMasterMultiSockets<Types>
{
protected:
    typedef typename Types::tlm_payload_type PayloadType;
    typedef typename Types::tlm_phase_type PhaseType;

public:
    /** Non blocking transport function pointer. */
    typedef tlm::tlm_sync_enum (Module::* NBFunc)(int, PayloadType&, PhaseType&);

protected:
    /** Proxy object implementing the bw transport interface. */
    class Proxy : public tlm::tlm_bw_transport_if<Types>
    {
    public:
        /** Owner of the proxy. */
        const MasterMultiSockets<Module, Types>& owner;

        /** Object and function(s) to which to defer interface calls. */
        Module& t;
        NBFunc bw;

        Proxy(const MasterMultiSockets<Module, Types>& owner_,
            Module& t_, NBFunc bw_) :
            owner(owner_), t(t_), bw(bw_)
        {}

        tlm::tlm_sync_enum nb_transport_bw(int id, PayloadType& trans,
            PhaseType& phase, sc_core::sc_time& delay)
        {
            return (t.*bw)(trans, phase);
        }

        void invalidate_direct_mem_ptr(sc_dt::uint64, sc_dt::uint64)
        {
            std::ostringstream message;
            message << owner.name() << ": DMI not implemented";
            SC_REPORT_ERROR("/ARM/TLM/SimpleMasterSocket",
                message.str().c_str());
        }
    };

    Proxy proxy;

public:
    MasterMultiSockets(const char* name_, Module& t, NBFunc bw,
        Protocol protocol_, unsigned port_width_) :
        BaseMasterMultiSockets<Types>(name_, protocol_, port_width_),
        proxy(*this, t, bw)
    {
        tlm_utils::multi_passthrough_initiator_socket<Types>::bind(proxy);
    }

    /** Convenience function for fw without time. */
    tlm::tlm_sync_enum nb_transport_fw(int id, PayloadType& trans, PhaseType& phase)
    {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        return (*this)->nb_transport_fw(id, trans, phase, delay);
    }
};

}
}

#endif // ARM_TLM_SOCKET_H
