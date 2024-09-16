#ifndef ARM_MONITOR_H
#define ARM_MONITOR_H

#include <map>

#include <ARM/TLM/arm_axi4.h>

class Monitor : public sc_core::sc_module
{
protected:
    SC_HAS_PROCESS(Monitor);

    /* Beat-sized data for data printing. */
    uint8_t* beat_data;

    /* Map of burst counts for observed Payloads. */
    std::map<ARM::AXI4::Payload*, unsigned> payload_burst_index;

    tlm::tlm_sync_enum nb_transport_fw(int id, ARM::AXI4::Payload& payload,
        ARM::AXI4::Phase& phase);
    tlm::tlm_sync_enum nb_transport_bw(int id, ARM::AXI4::Payload& payload,
        ARM::AXI4::Phase& phase);

    void print_payload(ARM::AXI4::Payload& payload,
        ARM::AXI4::Phase sent_phase, tlm::tlm_sync_enum reply,
        ARM::AXI4::Phase reply_phase);

public:
    Monitor(sc_core::sc_module_name name, unsigned port_width = 128);
    ~Monitor();

    ARM::AXI4::SlaveMultiSockets<Monitor> slave;
    ARM::AXI4::MasterMultiSockets<Monitor> master;
};

#endif // ARM_MONITOR_H
