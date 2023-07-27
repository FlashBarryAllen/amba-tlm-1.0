//-------------------------------------------------------------------
// The Clear BSD License
//
// Copyright (c) 2015-2019 Arm Limited.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted (subject to the limitations in the disclaimer
// below) provided that the following conditions are met:
//
//      * Redistributions of source code must retain the above copyright notice,
//      this list of conditions and the following disclaimer.
//
//      * Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//      * Neither the name of the copyright holder nor the names of its
//      contributors may be used to endorse or promote products derived from this
//      software without specific prior written permission.
//
// NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED BY
// THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
// CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
// PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
// BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
// IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//-------------------------------------------------------------------

#include <stdint.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <new>
#include <string>
#include <vector>

#include <ARM/TLM/arm_axi4_payload.h>

#ifdef ARM_TLM_ENABLE_VALGRIND
#include <valgrind/memcheck.h>
#else
#define VALGRIND_CREATE_MEMPOOL(d1, d2, d3) do {} while (0)
#define VALGRIND_DESTROY_MEMPOOL(d1) do {} while (0)
#define VALGRIND_MEMPOOL_ALLOC(d1, d2, d3) do {} while (0)
#define VALGRIND_MEMPOOL_FREE(d1, d2) do {} while (0)
#endif

#ifndef ARM_TLM_EXPORT
#define ARM_TLM_EXPORT
#endif

namespace ARM
{
namespace AXI4
{

ARM_TLM_EXPORT ARM_AXI4_TLM_API_VERSION::ARM_AXI4_TLM_API_VERSION(){}

/**
 * Correct a beat index for a wrapping burst to give an index in terms of the
 * address order of beats rather than (expected) arrival order.
 */
#define BURST_BEAT(IDX) \
    ((get_burst() == BURST_WRAP) \
        ? ((IDX + (address >> get_size())) & get_len()) \
        : IDX)

/**
 * Offset from the bottom of signal-level data where the base of the given
 * beat can be found. BEAT is the beat index counting from the lowest beat
 * address upwards rather than the arrival order of the beat.
 */
#define RAW_OFFSET(BEAT) \
    ((address + (BEAT << get_size())) & ((1 << width) - (1 << get_size())))

/**
 * Data part of payload. This is more complicated that the
 * tlm_generic_payload's data management as the aim is to make data sharable
 * between Payload objects but for that data to be managed and for data beats to
 * be tracked.
 */
class PayloadData
{
public:
    /** Reference count similar to Payload's reference counting mechanism. */
    mutable unsigned refcount;

    /**
     * If true: the data is longer than 64 bytes and must be allocated and
     * managed as data_ptr. If false: data will fit in the array data_short. 64
     * bytes is significant as it is a typical cache line length and so a common
     * transaction data length.
     */
    bool long_data;

    /**
     * If true: the byte write strobes are longer than 64 bits and managed as
     * strobe_ptr. If false: strobes will fit into strobe_short. For read
     * transactions, strobe_ptr/strobe_short are used to store beat responses
     * and so the length of strobes and data may not be the same.
     */
    bool long_strobe;

    /** Whole transaction response. */
    Resp resp;

    /** Whole transaction command. */
    const Command command;

    /** AXI4 LEN field. Burst length - 1. */
    const uint8_t len;

    /** AXI4 SIZE field. log_2(Burst data length in bytes). */
    const Size size;

    /** AXI4 BURST field. Burst mode. */
    const Burst burst;

    /** Number of beats written into this PayloadData. */
    uint16_t beats_complete;

    /** Union of strobe long/short arrays. See long_strobe. */
    union
    {
        uint8_t strobe_short[8];
        uint8_t* strobe_ptr;
    };

    /** Union of data long/short arrays. See long_data. */
    union
    {
        uint8_t data_short[64];
        uint8_t* data_ptr;
    };

    /**
     * Create a PayloadData with fixed fields required for interpreting data
     * organization.
     */
    PayloadData(Command command_, Size size_, uint8_t len_,
        Burst burst_ = BURST_INCR);

    ~PayloadData();

    /** Allocate PayloadData in the PayloadPool. */
    static void* operator new (size_t size);

    /** Free a PayloadData once refcount == 0. */
    static void operator delete (void* p);

    /** Increment reference count. */
    void ref();

    /** Decrement reference count. */
    void unref();

    /**
     * Copy out data to bytes of the given array writing only the bytes which
     * are selected by the write strobes.
     */
    void strobe_out_data(uint8_t* dst, unsigned offset, unsigned length);

    /** Copy data out of this object into the given array. */
    void copy_out_data(uint8_t* dst, unsigned offset, unsigned length);

    /** Copy data into this object from the given array. */
    void copy_in_data(const uint8_t* src, unsigned offset, unsigned length);

    /**
     * Copy out strobe bytes into the given array. Note that strobes are packed
     * into bits for writes and so this function's offset and length arguments
     * need to be appropriately scaled. For read responses, offset corresponds
     * to a beat index.
     */
    void copy_out_strobe(uint8_t* dst, unsigned offset, unsigned length);

    /** Copy in strobe bytes into the given array. */
    void copy_in_strobe(const uint8_t* src, unsigned offset, unsigned length);

    /** Fill strobe bytes with the given byte. */
    void fill_strobe(uint8_t value, unsigned offset, unsigned length);
};

/**
 * Source of all allocated Payloads. The PayloadPool manages the memory of
 * Payloads, PayloadDatas and issuing unique IDs to Payloads. PayloadPool never
 * deallocates any memory for Payload and PayloadData object (other than when
 * PayloadData's contain long data) but maintains free lists of objects returned
 * to the pool from which new objects are created.
 */
class PayloadPool
{
private:
    /** The unique ID of the next Payload to be created. */
    uint64_t next_uid;

    /**
     * Size of the Payload object will all registered extensions. All Payloads
     * will have the same size.
     */
    unsigned payload_size;

    /**
     * Set to true once the first Payload is requested. Extensions may not be
     * registered once pool_fixed becomes true.
     */
    bool pool_fixed;

    /** Number of allocated Payloads in existence. */
    std::size_t allocated_payload_count;

    /** Number of allocated PayloadDatas in existence. */
    std::size_t allocated_payload_data_count;

    /** LIFO free list of allocated Payload objects. */
    std::vector<Payload*> payload_pool;

    /** LIFO free list of allocated PayloadData objects. */
    std::vector<PayloadData*> payload_data_pool;

    /** ExtensionEntry class holidng an extensuion manager and offset */
    class ExtensionEntry
    {
    public:
        std::size_t offset;
        PayloadExtensionManager* manager;
        ExtensionEntry(std::size_t offset_, PayloadExtensionManager* manager_):
            offset(offset_),
            manager(manager_)
        {}
        ExtensionEntry(){}
    };
    /** Map of extension names to ExtensionEntries */
    std::map<std::string, ExtensionEntry> extension_map;

    /** Debug allocation of new payloads. */
    bool debug_unique;
    bool debug_always_free;

public:
    /** Registered new/delete for allocations within the pool. */
    void* (*local_malloc)(size_t size);
    void (*local_free)(void* ptr);

    /** Return the next unique ID and advance next_id. */
    uint64_t get_uid() { return next_uid++; }

    /** Return a Payload to the payload free list. */
    void free_payload(Payload* payload)
    {
        for (std::map<std::string, ExtensionEntry>::iterator it = 
            extension_map.begin(); it!=extension_map.end(); ++it)
        {
            ExtensionEntry ext = it->second;
            ext.manager->destroy(
                reinterpret_cast<char*>(payload) + ext.offset);
        }
        VALGRIND_MEMPOOL_FREE(payload, payload);
        VALGRIND_DESTROY_MEMPOOL(payload);
        if (debug_always_free)
        {
            local_free(payload);
            allocated_payload_count--;

        } else if (!debug_unique)
        {
            payload_pool.push_back(payload);
        }
    }

    /** Return a PayloadData to the payload data free list. */
    void free_payload_data(PayloadData* payload_data)
    {
        VALGRIND_MEMPOOL_FREE(payload_data, payload_data);
        VALGRIND_DESTROY_MEMPOOL(payload_data);
        if (debug_always_free)
        {
            local_free(payload_data);
            allocated_payload_data_count--;

        } else if (!debug_unique)
        {
            payload_data_pool.push_back(payload_data);
        }
    }

    PayloadPool() :
        next_uid(1),
        /* payload_size wil grow as extensions are added. */
        payload_size(sizeof(Payload)),
        pool_fixed(false),
        allocated_payload_count(0),
        allocated_payload_data_count(0),
        debug_unique(false),
        debug_always_free(false),
        local_malloc(std::malloc),
        local_free(std::free)
    {
        char* env_val = getenv("ARM_TLM_DEBUG_ALLOC");
        if (env_val)
        {
            debug_unique = (std::string(env_val) == "UNIQUE");
            debug_always_free = (std::string(env_val) == "ALWAYS_FREE");
        }
    }

    /**
     * Create a new Payload either by allocating a new object or recycling a
     * Payload from the payload free list.
     */
    Payload* new_payload(const Payload* parent = NULL)
    {
        Payload* payload;

        if (payload_pool.empty())
        {
            pool_fixed = true;
            payload = reinterpret_cast<Payload*>(local_malloc(payload_size));
            allocated_payload_count++;
        } else
        {
            payload = payload_pool.back();
            payload_pool.pop_back();
        }

        VALGRIND_CREATE_MEMPOOL(payload, 0, 0);
        VALGRIND_MEMPOOL_ALLOC(payload, payload, payload_size);

        /* Copy the parent or clear all the payload's data members. */
        if (parent)
        {
            std::memcpy(payload, parent, payload_size);
            for (std::map<std::string, ExtensionEntry>::iterator it =
                extension_map.begin(); it!=extension_map.end(); ++it)
            {
                ExtensionEntry &ext = it->second;

                ext.manager->copy(
                    reinterpret_cast<char*>(payload) + ext.offset,
                    reinterpret_cast<const char*>(parent) + ext.offset);
            }
        }
        else
        {
            std::memset(payload, 0, payload_size);
            for (std::map<std::string, ExtensionEntry>::iterator it =
                extension_map.begin(); it!=extension_map.end(); ++it)
            {
                ExtensionEntry ext = it->second;
                ext.manager->create(
                    reinterpret_cast<char*>(payload) + ext.offset);
            }
        }

        /*
         * The created payload must still be placement constructed to
         * initialize its required data members.
         */
        return payload;
    }

    /**
     * Create a new PayloadData either by allocating a new object or recycling a
     * PayloadData from the payload data free list.
     */
    PayloadData* new_payload_data()
    {
        PayloadData* payload_data;

        if (payload_data_pool.empty())
        {
            payload_data = reinterpret_cast<PayloadData*>(
                local_malloc(sizeof(PayloadData)));
            allocated_payload_data_count++;
        } else
        {
            payload_data = payload_data_pool.back();
            payload_data_pool.pop_back();
        }

        VALGRIND_CREATE_MEMPOOL(payload_data, 0, 0);
        VALGRIND_MEMPOOL_ALLOC(payload_data, payload_data, payload_size);

        /*
         * The created payload data must still be placement constructed to
         * initialize its required data members.
         */
        return payload_data;
    }

    /**
     * Register or find an extension's byte offset within Payload objects. The
     * first call with any particular name will grow payload_size and register
     * the offset of required extension with extension_map.
     *
     * This function can only be called before the PayloadPool has been fixed at
     * its first Payload creation event.
     */
    
    class PayloadExtensionManagerNop:
        public PayloadExtensionManager
    {
    public:
        std::size_t get_size() { return 0; }
    };
    
    std::size_t get_extension_offset(unsigned size, const char* name)
    {
        std::map<std::string, ExtensionEntry>::iterator it =
            extension_map.find(name);

        if (it == extension_map.end())
        {
            assert(pool_fixed == false);
            /* 32 bit align the payload_size. */
            std::size_t extension_offset = (payload_size + 3) & ~3;

            extension_map[name] = ExtensionEntry(extension_offset, new PayloadExtensionManagerNop);
            payload_size = extension_offset + size;

            return extension_offset;
        }
        return it->second.offset;
    }

    std::size_t get_extension_offset(const char* name)
    {
        std::map<std::string, ExtensionEntry>::iterator it =
            extension_map.find(name);
        if (it == extension_map.end())
            return 0;
        return it->second.offset;
    }
    
    std::size_t register_extension(const char* name, PayloadExtensionManager* manager)
    {
        assert(pool_fixed == false);
        /* 32 bit align the payload_size. */
        std::size_t extension_offset = (payload_size + 3) & ~3;

        extension_map[name] = ExtensionEntry(extension_offset, manager);
        payload_size = extension_offset + manager->get_size();

        return extension_offset;
    }

    /** Dump debugging info. */
    void debug(std::ostream& stream);
};

/**
 * Global variable. Must not be replicated between code/libraries in the same
 * program.
 */
PayloadPool* global_payload_pool = NULL;

/** Make the global pool on request. */
PayloadPool* get_global_pool()
{
    if (global_payload_pool == NULL)
        global_payload_pool = new PayloadPool();
    return global_payload_pool;
}

Payload::Payload(const Payload& other) :
    uid(get_global_pool()->get_uid()),
    parent(NULL),
    payload_data(NULL)
{
    /*
     * This implementation should never be called but sets the const members
     * of Payload to avoid compilation errors.
     */
}

Payload::Payload(PayloadData* payload_data_, uint64_t address_,
    Payload* parent_) :
    refcount(1),
    uid(get_global_pool()->get_uid()),
    parent(parent_),
    payload_data(payload_data_),
    address(address_),
    lock(parent_->lock),
    cache(parent_->cache),
    prot(parent_->prot),
    snoop(parent_->snoop),
    domain(parent_->domain),
    bar(parent_->bar),
    atop(parent_->atop)
{
    payload_data->ref();
    parent->ref();
}

Payload::Payload(Command command, uint64_t address_, Size size,
    uint8_t len, Burst burst) :
    refcount(1),
    uid(get_global_pool()->get_uid()),
    parent(NULL),
    payload_data(new PayloadData(command, size, len, burst)),
    address(address_)
{}

Payload::~Payload()
{
    assert(refcount == 0);
    payload_data->unref();
    if (parent)
        parent->unref();
}

ARM_TLM_EXPORT Payload* Payload::new_payload(Command command, uint64_t address, Size size,
    uint8_t len, Burst burst)
{
    Payload* payload = get_global_pool()->new_payload();

    return new (payload) Payload(command, address, size, len, burst);
}

ARM_TLM_EXPORT void Payload::operator delete (void* p)
{
    get_global_pool()->free_payload(reinterpret_cast<Payload*>(p));
}

ARM_TLM_EXPORT Payload* Payload::clone()
{
    Payload* payload = get_global_pool()->new_payload(this);
    new (payload) Payload(payload_data, address, this);

    return payload;
}

ARM_TLM_EXPORT Payload* Payload::descend(Command command, uint64_t address_, Size size,
    uint8_t len, Burst burst)
{
    Payload* payload = get_global_pool()->new_payload(this);
    PayloadData* new_payload_data = new PayloadData(command, size, len, burst);
    new (payload) Payload(new_payload_data, address_, this);
    /* Correct for ref() of payload data in Payload constructor. */
    new_payload_data->unref();

    return payload;
}

ARM_TLM_EXPORT void Payload::ref() const
{
    assert(refcount != 0);
    refcount++;
}

ARM_TLM_EXPORT void Payload::unref() const
{
    assert(refcount != 0);
    refcount--;
    if (refcount == 0)
        delete this;
}

ARM_TLM_EXPORT std::size_t Payload::get_beat_data_length() const
{
    return 1 << get_size();
}

ARM_TLM_EXPORT std::size_t Payload::get_data_length() const
{
    return get_beat_count() << get_size();
}

ARM_TLM_EXPORT uint64_t Payload::get_base_address() const
{
    uint64_t element_size = get_beat_data_length();

    if (get_burst() == BURST_WRAP)
        element_size *= get_beat_count();

    /* Mask off address bits below the element size. */
    return address & ~(element_size - 1);
}

ARM_TLM_EXPORT uint64_t Payload::get_address() const
{
    return address;
}

ARM_TLM_EXPORT void Payload::set_address(uint64_t new_address)
{
    if (get_burst() == BURST_WRAP)
    {
        uint64_t mask = get_len() << get_size();
        assert((address & mask) == (new_address & mask));
    }
    address = new_address;
}

ARM_TLM_EXPORT uint32_t Payload::get_id() const
{
    return id;
}

ARM_TLM_EXPORT void Payload::set_id(uint32_t src_id, uint32_t dst_id)
{
    id |= (src_id << 0x1c) | (dst_id << 0x18);
}

ARM_TLM_EXPORT Resp Payload::get_resp() const
{
    return payload_data->resp;
}

ARM_TLM_EXPORT void Payload::set_resp(Resp new_resp)
{
    payload_data->resp = new_resp;
}

ARM_TLM_EXPORT Command Payload::get_command() const
{
    return payload_data->command;
}

ARM_TLM_EXPORT uint8_t Payload::get_len() const
{
    return payload_data->len;
}

ARM_TLM_EXPORT Size Payload::get_size() const
{
    return payload_data->size;
}

ARM_TLM_EXPORT Burst Payload::get_burst() const
{
    return payload_data->burst;
}

ARM_TLM_EXPORT unsigned Payload::get_beat_count() const
{
    return payload_data->len + 1;
}

ARM_TLM_EXPORT unsigned Payload::get_beats_complete() const
{
    return payload_data->beats_complete;
}

ARM_TLM_EXPORT void Payload::read_in(const uint8_t* data, Resp* resp_arr)
{
    std::size_t data_length = get_data_length();

    payload_data->copy_in_data(data, 0, data_length);

    if (resp_arr)
    {
        set_resp(resp_arr[0]);
        for (unsigned i = 1; i < get_beat_count(); i++)
        {
            if (resp_arr[i] != get_resp())
                set_resp(RESP_INCONSISTENT);
        }

        if (get_resp() == RESP_INCONSISTENT)
        {
            payload_data->copy_in_strobe(
                reinterpret_cast<uint8_t*>(resp_arr), 0,
                get_beat_count());
        }
    } else
    {
        set_resp(RESP_OKAY);
    }
    payload_data->beats_complete = get_beat_count();
}

ARM_TLM_EXPORT void Payload::read_out(uint8_t* data) const
{
    assert(payload_data->beats_complete == get_beat_count());

    payload_data->copy_out_data(data, 0, get_data_length());
}

ARM_TLM_EXPORT void Payload::read_out_resps(Resp* dest) const
{
    assert(payload_data->beats_complete == get_beat_count());

    if (get_resp() == ARM::AXI4::RESP_INCONSISTENT)
    {
        payload_data->copy_out_strobe(reinterpret_cast<uint8_t*>(dest),
            0, get_beat_count());
    } else
    {
        std::memset(dest, get_resp(), get_beat_count());
    }
}

ARM_TLM_EXPORT void Payload::write_in(const uint8_t* data, const uint8_t* strobe)
{
    std::size_t data_length = get_data_length();

    payload_data->copy_in_data(data, 0, data_length);

    if (strobe != NULL)
        payload_data->copy_in_strobe(strobe, 0, (data_length + 7) / 8);
    else
        payload_data->fill_strobe(0xFF, 0, (data_length + 7) / 8);

    payload_data->beats_complete = get_beat_count();
}

ARM_TLM_EXPORT void Payload::write_out(uint8_t* data) const
{
    assert(payload_data->beats_complete == get_beat_count());

    payload_data->strobe_out_data(data, 0, get_data_length());
}

ARM_TLM_EXPORT void Payload::write_out_strobes(uint8_t* strobes) const
{
    assert(payload_data->beats_complete == get_beat_count());
    std::size_t data_length = get_data_length();

    payload_data->copy_out_strobe(strobes, 0, (data_length + 7) / 8);
}

ARM_TLM_EXPORT void Payload::snoop_in(const uint8_t* data)
{
    payload_data->copy_in_data(data, 0, get_data_length());
    payload_data->beats_complete = get_beat_count();
}

ARM_TLM_EXPORT void Payload::snoop_out(uint8_t* data) const
{
    assert(payload_data->beats_complete == get_beat_count());

    payload_data->copy_out_data(data, 0, get_data_length());
}

ARM_TLM_EXPORT void Payload::read_in_beat(const uint8_t* data, Resp resp_in)
{
    assert(payload_data->beats_complete < get_beat_count());

    unsigned beat_index = BURST_BEAT(payload_data->beats_complete);
    unsigned element_size = get_beat_data_length();;

    if (payload_data->beats_complete == 0)
        set_resp(resp_in);

    payload_data->copy_in_data(data, beat_index * element_size, element_size);
    if (get_resp() != resp_in)
    {
        if (get_resp() != RESP_INCONSISTENT)
        {
            payload_data->fill_strobe(get_resp(), 0, get_beat_count());
            set_resp(RESP_INCONSISTENT);
        }
        payload_data->fill_strobe(resp_in, beat_index, 1);
    }
    payload_data->beats_complete++;
}

ARM_TLM_EXPORT void Payload::read_out_beat(unsigned beat_index, uint8_t* data) const
{
    assert(beat_index < payload_data->beats_complete);

    beat_index = BURST_BEAT(beat_index);
    payload_data->copy_out_data(data, beat_index << get_size(),
        get_beat_data_length());
}

ARM_TLM_EXPORT Resp Payload::read_out_beat_resp(unsigned beat_index) const
{
    assert(beat_index < payload_data->beats_complete);
    beat_index = BURST_BEAT(beat_index);

    if (get_resp() == RESP_INCONSISTENT)
    {
        uint8_t reply;

        payload_data->copy_out_strobe(&reply, beat_index, 1);
        return reply;
    } else
    {
        return get_resp();
    }
}

ARM_TLM_EXPORT void Payload::write_in_beat(const uint8_t* data, uint64_t strobe)
{
    assert(get_size() <= SIZE_64);
    write_in_beat(data, (const uint8_t*)&strobe);
}

ARM_TLM_EXPORT void Payload::write_in_beat(const uint8_t* data, const uint8_t* strobe)
{
    assert(payload_data->beats_complete < get_beat_count());

    unsigned beat_index = BURST_BEAT(payload_data->beats_complete);
    unsigned element_size = get_beat_data_length();

    payload_data->copy_in_data(data, beat_index * element_size, element_size);

    if (element_size < 8)
    {
        uint8_t stored_strobe = 0;
        unsigned index = beat_index * element_size;
        unsigned byte_index = index / 8;
        unsigned bit_index = index % 8;

        if (bit_index != 0)
            payload_data->copy_out_strobe(&stored_strobe, byte_index, 1);

        uint8_t strobe_in;

        if (strobe)
            strobe_in = *strobe;
        else
            strobe_in = 0xFF;

        stored_strobe |= (strobe_in & ((1 << element_size) - 1)) << bit_index;
        payload_data->fill_strobe(stored_strobe, byte_index, 1);
    } else
    {
        unsigned byte_index = (beat_index * element_size) / 8;

        if (strobe)
            payload_data->copy_in_strobe(strobe, byte_index, element_size / 8);
        else
            payload_data->fill_strobe(0xFF, byte_index, element_size / 8);
    }
    payload_data->beats_complete++;
}

ARM_TLM_EXPORT void Payload::write_out_beat(unsigned beat_index, uint8_t* data) const
{
    assert(beat_index < payload_data->beats_complete);
    beat_index = BURST_BEAT(beat_index);
    payload_data->strobe_out_data(data, beat_index << get_size(),
        get_beat_data_length());
}

ARM_TLM_EXPORT uint64_t Payload::write_out_beat_strobe(unsigned beat_index) const
{
    assert(get_size() <= SIZE_64);
    uint64_t reply = 0;

    /* Note that this assumes that the host is little endian. */
    write_out_beat_strobe(beat_index, reinterpret_cast<uint8_t*>(&reply));

    return reply;
}

ARM_TLM_EXPORT void Payload::write_out_beat_strobe(unsigned beat_index, uint8_t* strobe) const
{
    assert(beat_index < payload_data->beats_complete);
    beat_index = BURST_BEAT(beat_index);
    unsigned index = beat_index << get_size();
    unsigned byte_index = index / 8;
    unsigned element_size = get_beat_data_length();

    if (element_size < 8)
    {
        unsigned bit_index = index % 8;
        uint8_t reply;

        payload_data->copy_out_strobe(&reply, byte_index, 1);
        reply >>= bit_index;
        reply &= (1 << element_size) - 1;
        *strobe = reply;
    } else
    {
        unsigned bytesize = element_size / 8;

        payload_data->copy_out_strobe(strobe, byte_index, bytesize);
    }
}

ARM_TLM_EXPORT void Payload::snoop_in_beat(const uint8_t* data)
{
    assert(payload_data->beats_complete < get_beat_count());

    unsigned beat_index = BURST_BEAT(payload_data->beats_complete);
    unsigned element_size = get_beat_data_length();

    payload_data->copy_in_data(data, beat_index * element_size, element_size);
    payload_data->beats_complete++;
}

ARM_TLM_EXPORT void Payload::snoop_out_beat(unsigned beat_index, uint8_t* data) const
{
    assert(beat_index < payload_data->beats_complete);
    beat_index = BURST_BEAT(beat_index);
    payload_data->copy_out_data(data, beat_index << get_size(),
        get_beat_data_length());
}

ARM_TLM_EXPORT void Payload::read_in_beat_raw(Size width, const uint8_t* data, Resp resp)
{
    assert(get_size() <= width);

    unsigned beat_index = BURST_BEAT(payload_data->beats_complete);
    unsigned offset = RAW_OFFSET(beat_index);

    read_in_beat(&data[offset], resp);
}

ARM_TLM_EXPORT void Payload::read_out_beat_raw(Size width, unsigned beat_index,
    uint8_t* data) const
{
    assert(get_size() <= width);
    assert(beat_index < payload_data->beats_complete);

    unsigned offset = RAW_OFFSET(BURST_BEAT(beat_index));

    read_out_beat(beat_index, &data[offset]);
}

ARM_TLM_EXPORT void Payload::write_in_beat_raw(Size width, const uint8_t* data,
    uint64_t strobe)
{
    assert(get_size() <= SIZE_64);
    assert(get_size() <= width);

    /* Note that this assumes that the host is little endian. */
    write_in_beat_raw(width, data, reinterpret_cast<uint8_t*>(&strobe));
}

ARM_TLM_EXPORT void Payload::write_in_beat_raw(Size width, const uint8_t* data,
    const uint8_t* strobe)
{
    assert(get_size() <= width);

    unsigned beat_index = BURST_BEAT(payload_data->beats_complete);
    unsigned offset = RAW_OFFSET(beat_index);

    if (get_size() < SIZE_8)
    {
        uint64_t strobe_byte = strobe[offset / 8];
        strobe_byte >>= offset % 8;
        strobe_byte &= (1 << get_beat_data_length()) - 1;
        write_in_beat(&data[offset], strobe_byte);
    } else
    {
        write_in_beat(&data[offset], &strobe[offset / 8]);
    }
}

ARM_TLM_EXPORT void Payload::write_out_beat_raw(Size width,
    unsigned beat_index, uint8_t* data) const
{
    assert(get_size() <= width);
    assert(beat_index < payload_data->beats_complete);

    unsigned offset = RAW_OFFSET(BURST_BEAT(beat_index));

    write_out_beat(beat_index, &data[offset]);
}

ARM_TLM_EXPORT uint64_t Payload::write_out_beat_raw_strobe(Size width,
    unsigned beat_index) const
{
    assert(get_size() <= SIZE_64);
    assert(get_size() <= width);

    unsigned offset = RAW_OFFSET(BURST_BEAT(beat_index));
    uint64_t reply = write_out_beat_strobe(beat_index);

    reply <<= offset;

    return reply;
}

ARM_TLM_EXPORT void Payload::write_out_beat_raw_strobe(Size width,
    unsigned beat_index, uint8_t* strobe) const
{
    assert(get_size() <= width);
    assert(beat_index < payload_data->beats_complete);

    unsigned offset = RAW_OFFSET(beat_index);

    /* Zero the whole width of strobe. */
    unsigned strobe_array_size = (width <= SIZE_8 ? 1 : (1 << width) / 8);
    std::memset(strobe, 0, strobe_array_size);

    if (get_size() < SIZE_8)
    {
        uint8_t strobe_byte = write_out_beat_strobe(beat_index);
        strobe_byte <<= offset % 8;
        strobe[offset / 8] = strobe_byte;
    } else
    {
        write_out_beat_strobe(beat_index, &strobe[offset / 8]);
    }
}

ARM_TLM_EXPORT std::size_t Payload::get_extension_offset(const char* name)
{
    return get_global_pool()->get_extension_offset(name);
}

ARM_TLM_EXPORT std::size_t Payload::register_extension(const char* name,
    PayloadExtensionManager* manager)
{
    return get_global_pool()->register_extension(name, manager);
}

void PayloadPool::debug(std::ostream& stream)
{
    stream << "Payloads free/allocated/in use:     "
        << payload_pool.size()
        << '/' << allocated_payload_count
        << '/' << allocated_payload_count - payload_pool.size() << '\n';

    stream << "PayloadDatas free/allocated/in use: "
        << payload_data_pool.size()
        << '/' << allocated_payload_data_count
        << '/' << allocated_payload_data_count
            - payload_data_pool.size() << '\n';
}

ARM_TLM_EXPORT void Payload::debug_payload_pool(std::ostream& stream)
{
    get_global_pool()->debug(stream);
}

PayloadData::PayloadData(Command command_, Size size_, uint8_t len_,
    Burst burst_) :
    refcount(1),
    long_data(false),
    long_strobe(false),
    command(command_),
    len(len_),
    size(size_),
    burst(burst_),
    beats_complete(0)
{
    assert(burst != BURST_WRAP || ((len & (len + 1)) == 0));
    std::size_t data_length = (len + 1) << size;

    if (data_length > 64)
    {
        data_ptr = reinterpret_cast<uint8_t*>(
            get_global_pool()->local_malloc(data_length));
        long_data = true;
    }

    unsigned strobe_length = 0;

    if (command == COMMAND_READ)
        strobe_length = len + 1;
    else if (command == COMMAND_WRITE)
        strobe_length = (data_length + 7) / 8;

    if (strobe_length > 8)
    {
        strobe_ptr = reinterpret_cast<uint8_t*>(
            get_global_pool()->local_malloc(strobe_length));
        long_strobe = true;
    }

}

PayloadData::~PayloadData()
{
    if (long_data)
        get_global_pool()->local_free(data_ptr);
    if (long_strobe)
        get_global_pool()->local_free(strobe_ptr);
}

void* PayloadData::operator new (std::size_t size)
{
    PayloadData* payload = get_global_pool()->new_payload_data();

    return payload;
}

void PayloadData::operator delete (void* p)
{
    get_global_pool()->free_payload_data(reinterpret_cast<PayloadData*>(p));
}

void PayloadData::unref()
{
    assert(refcount != 0);
    refcount--;
    if (refcount == 0)
        delete this;
}

void PayloadData::ref()
{
    refcount++;
}

void PayloadData::strobe_out_data(uint8_t* dst, unsigned offset,
    unsigned length)
{
    uint8_t* strobe_base;
    uint8_t* data_base;

    if (long_data)
        data_base = data_ptr;
    else
        data_base = data_short;

    if (long_strobe)
        strobe_base = strobe_ptr;
    else
        strobe_base = strobe_short;

    uint8_t strobe = strobe_base[offset / 8];
    unsigned i = 0;

    while (true)
    {
        if (strobe & (1 << ((offset + i) % 8)))
            dst[i] = data_base[offset + i];
        i++;
        if (i >= length)
            break;
        if (((offset + i) % 8) == 0)
            strobe = strobe_base[(offset + i) / 8];
    }
}

void PayloadData::copy_out_data(uint8_t* dst, unsigned offset,
    unsigned length)
{
    uint8_t* data_base;

    if (long_data)
        data_base = data_ptr;
    else
        data_base = data_short;
    std::memcpy(dst, &data_base[offset], length);
}

void PayloadData::copy_in_data(const uint8_t* src, unsigned offset,
    unsigned length)
{
    uint8_t* data_base;

    if (long_data)
        data_base = data_ptr;
    else
        data_base = data_short;
    std::memcpy(&data_base[offset], src, length);
}

void PayloadData::copy_out_strobe(uint8_t* dst, unsigned offset,
    unsigned length)
{
    uint8_t* strobe_base;

    if (long_strobe)
        strobe_base = strobe_ptr;
    else
        strobe_base = strobe_short;
    std::memcpy(dst, &strobe_base[offset], length);
}

void PayloadData::copy_in_strobe(const uint8_t* src, unsigned offset,
    unsigned length)
{
    uint8_t* strobe_base;

    if (long_strobe)
        strobe_base = strobe_ptr;
    else
        strobe_base = strobe_short;
    std::memcpy(&strobe_base[offset], src, length);
}

void PayloadData::fill_strobe(uint8_t value, unsigned offset,
    unsigned length)
{
    uint8_t* strobe_base;

    if (long_strobe)
        strobe_base = strobe_ptr;
    else
        strobe_base = strobe_short;

    std::memset(&strobe_base[offset], value, length);
}

}
}
