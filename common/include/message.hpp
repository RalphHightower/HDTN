#ifndef _HDTN_MSG_H
#define _HDTN_MSG_H

#include <stdint.h>

#include "stats.hpp"
#include "codec/bpv6.h"

#define HMSG_MSG_MAX (65536)
#define CHUNK_SIZE (65536 * 100) //TODO

#define HDTN_FLAG_CUSTODY_REQ (0x01)
#define HDTN_FLAG_CUSTODY_OK (0x02)
#define HDTN_FLAG_CUSTODY_FAIL (0x04)

// Common message types shared by all components
#define HDTN_MSGTYPE_EGRESS (0x0004)
#define HDTN_MSGTYPE_STORE (0x0005)
#define HDTN_MSGTYPE_EGRESS_ADD_OPPORTUNISTIC_LINK (0x0006)
#define HDTN_MSGTYPE_EGRESS_REMOVE_OPPORTUNISTIC_LINK (0x0007)
#define HDTN_MSGTYPE_STORAGE_ADD_OPPORTUNISTIC_LINK (0x0008)
#define HDTN_MSGTYPE_STORAGE_REMOVE_OPPORTUNISTIC_LINK (0x0009)

// Egress Messages range is 0xE000 to 0xEAFF
#define HDTN_MSGTYPE_ENOTIMPL (0xE000)  // convergence layer type not  // implemented

// Command and control messages accepted by the storage component - range is
// 0xF000 to 0xFAFF
#define HDTN_MSGTYPE_COK (0xF000)         // acknowledgement that previous command was processed successfully
#define HDTN_MSGTYPE_CFAIL (0xF001)       // negative acknowledgement of previous command
#define HDTN_MSGTYPE_CTELEM_REQ (0xF002)  // request for telemetry from the application
#define HDTN_MSGTYPE_CSCHED_REQ (0xF003)  // request for a scheduled event

// Telemetry messages - range is 0xFB00 to 0xFBFF
#define HDTN_MSGTYPE_TSTORAGE (0xFB00)  // response that indicates telemetry is of type "storage"

// Internal messages used only by the storage component - types start at 0xFC00
#define HDTN_MSGTYPE_IOK (0xFC00)  // indicates successful worker startup
#define HDTN_MSGTYPE_IABORT \
    (0xFC01)  // indicates that the worker encountered a critical failure and will // immediately terminate
#define HDTN_MSGTYPE_ISHUTDOWN (0xFC02)   // tells the worker to shut down
#define HDTN_MSGTYPE_ILINKUP (0xFC03)     // Link available event from scheduler
#define HDTN_MSGTYPE_ILINKDOWN (0xFC04)    // Link unavailable event from scheduler
#define HDTN_MSGTYPE_IPRELOAD (0xFC05)    // preloads data because an event is scheduled to begin soon
#define HDTN_MSGTYPE_IWORKSTATS (0xFC06)  // update on worker stats sent from worker to parent

#define HDTN_MSGTYPE_ROUTEUPDATE (0xFC07) //Route Update Event from Router process

#define HDTN_MSGTYPE_LINKSTATUS (0xFC08) //Link Status Update Event from Egress process 

#define CPM_NEW_CONTACT_PLAN (0xFC09) //Reload with new contact plan message

#define HDTN_MSGTYPE_EGRESS_FAILED_BUNDLE_TO_STORAGE (0x5554)
#define HDTN_MSGTYPE_EGRESS_ACK_TO_STORAGE (0x5555)
#define HDTN_MSGTYPE_EGRESS_ACK_TO_INGRESS (0x5556)
#define HDTN_MSGTYPE_STORAGE_ACK_TO_INGRESS (0x5557)
#define HDTN_MSGTYPE_ALL_OUTDUCT_CAPABILITIES_TELEMETRY (0x5558)

namespace hdtn {
//#pragma pack (push, 1)
struct CommonHdr {
    uint16_t type;
    uint16_t flags;

    bool operator==(const CommonHdr & o) const {
        return (type == o.type) && (flags == o.flags);
    }
};


struct ToEgressHdr {
    CommonHdr base;
    uint8_t hasCustody;
    uint8_t isCutThroughFromIngress;
    uint8_t isOpportunisticFromStorage;
    uint8_t isCutThroughFromStorage;
    uint64_t nextHopNodeId;
    cbhe_eid_t finalDestEid;
    uint64_t custodyId;
    uint64_t outductIndex;

    //bool operator==(const ToEgressHdr & o) const {
    //    return (base == o.base) && (custodyId == o.custodyId);
    //}
};

struct EgressAckHdr {
    CommonHdr base;
    uint8_t error;
    uint8_t deleteNow; //set if message does not request custody (can be deleted after egress sends it)
    uint8_t isToStorage;
    uint8_t isResponseToStorageCutThrough;
    uint8_t isOpportunisticFromStorage;
    uint8_t unusedPadding[7];
    uint64_t nextHopNodeId;
    cbhe_eid_t finalDestEid;
    uint64_t custodyId;
    uint64_t outductIndex;

    //bool operator==(const EgressAckHdr & o) const {
    //    return (base == o.base) && (custodyId == o.custodyId);
    //}
};

struct ToStorageHdr {
    CommonHdr base;
    uint8_t dontStoreBundle;
    uint8_t isCustodyOrAdminRecord; //if no custody, storage just needs to decode primary header because ingress already verified the bundle
    uint8_t unused3;
    uint8_t unused4;
    uint64_t ingressUniqueId;
    uint64_t outductIndex; //for bundle pipeline limiting on a per outduct basis
    cbhe_eid_t finalDestEid; //for assisting storage on cut-through so it doesn't have to 
};

struct StorageAckHdr {
    CommonHdr base;
    uint8_t error;
    uint8_t unused1;
    uint8_t unused2;
    uint8_t unused3;
    uint64_t ingressUniqueId;
    uint64_t outductIndex; //for bundle pipeline limiting on a per outduct basis
};

struct TelemStorageHdr {
    CommonHdr base;
    StorageStats stats;
};

struct CscheduleHdr {
    CommonHdr base;
    uint32_t flowId;   // flow ID
    uint64_t rate;      // bytes / sec
    uint64_t offset;    // msec
    uint64_t duration;  // msec
};

struct IreleaseChangeHdr {
    uint64_t subscriptionBytes;
    CommonHdr base; //types ILINKDOWN or ILINKUP
    uint8_t unused1;
    uint8_t unused2;
    uint8_t unused3;
    uint8_t unused4;
    uint64_t outductArrayIndex; //outductUuid
    uint64_t rate;      // bytes / sec (start events only)
    uint64_t duration;  // msec (start events only)
    uint64_t prevHopNodeId;
    uint64_t nextHopNodeId;
    uint64_t time;
    uint64_t contact; //stop events only
};

struct RouteUpdateHdr {
    CommonHdr base;
    uint8_t unused3;
    uint8_t unused4;
    uint64_t nextHopNodeId;
    uint64_t finalDestNodeId;
    uint64_t route[20]; //optimal route
};

struct LinkStatusHdr {
    CommonHdr base;
    uint64_t event;
    uint64_t uuid;
    uint64_t unixTimeSecondsSince1970;
};

struct ContactPlanReloadHdr {
    CommonHdr base;
    uint8_t using_unix_timestamp;
    uint8_t unusedPadding[3];
 };

};  // namespace hdtn
//#pragma pack (pop)
#endif
