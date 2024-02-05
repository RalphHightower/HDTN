/**
 * @file BpSecPolicyManager.cpp
 * @author  Nadia Kortas <nadia.kortas@nasa.gov>
 * @author  Brian Tomko <brian.j.tomko@nasa.gov>
 *
 * @copyright Copyright  2021 United States Government as represented by
 * the National Aeronautics and Space Administration.
 * No copyright is claimed in the United States under Title 17, U.S.Code.
 * All Other Rights Reserved.
 *
 * @section LICENSE
 * Released under the NASA Open Source Agreement (NOSA)
 * See LICENSE.md in the source root directory for more information.
 */

#include "BpSecPolicyManager.h"
#include <boost/make_unique.hpp>
#include <boost/algorithm/string/trim.hpp>
#include "Uri.h"
#include "BinaryConversions.h"
#include "Logger.h"

static constexpr hdtn::Logger::SubProcess subprocess = hdtn::Logger::SubProcess::none;

BpSecPolicy::BpSecPolicy() :
    m_doIntegrity(false),
    m_doConfidentiality(false),
    //fields set by ValidateAndFinalize()
    m_bcbTargetsPayloadBlock(false),
    m_bibMustBeEncrypted(false),
    //integrity only variables
    m_integrityVariant(COSE_ALGORITHMS::HMAC_384_384),
    m_integrityScopeMask(BPSEC_BIB_HMAC_SHA2_INTEGRITY_SCOPE_MASKS::ALL_FLAGS_SET),
    m_bibCrcType(BPV7_CRC_TYPE::NONE),
    m_bibBlockTypeTargets(),
    m_hmacKeyEncryptionKey(),
    m_hmacKey(),
    m_integritySecurityFailureEventSetReferencePtr(NULL),
    //confidentiality only variables
    m_confidentialityVariant(COSE_ALGORITHMS::A256GCM),
    m_use12ByteIv(true),
    m_aadScopeMask(BPSEC_BCB_AES_GCM_AAD_SCOPE_MASKS::ALL_FLAGS_SET),
    m_bcbCrcType(BPV7_CRC_TYPE::NONE),
    m_bcbBlockTypeTargets(),
    m_confidentialityKeyEncryptionKey(),
    m_dataEncryptionKey(),
    m_confidentialitySecurityFailureEventSetReferencePtr(NULL) {}
bool BpSecPolicy::ValidateAndFinalize() {
    m_bcbTargetsPayloadBlock = false;
    m_bibMustBeEncrypted = false;

    if (m_doConfidentiality) {
        for (FragmentSet::data_fragment_set_t::const_iterator it = m_bcbBlockTypeTargets.cbegin();
            it != m_bcbBlockTypeTargets.cend(); ++it)
        {
            const FragmentSet::data_fragment_t& df = *it;
            for (uint64_t blockType = df.beginIndex; blockType <= df.endIndex; ++blockType) {
                if (blockType == static_cast<uint64_t>(BPV7_BLOCK_TYPE_CODE::PAYLOAD)) {
                    m_bcbTargetsPayloadBlock = true;
                    break;
                }
            }
        }
    }

    if (m_doIntegrity && m_doConfidentiality) {
        //When adding a BCB to a bundle, if some (or all) of the security
        //targets of the BCB match all of the security targets of an
        //existing BIB, then the existing BIB MUST also be encrypted.
        m_bibMustBeEncrypted = FragmentSet::FragmentSetsHaveOverlap(m_bcbBlockTypeTargets, m_bibBlockTypeTargets);
        if (m_bibMustBeEncrypted) {
            const bool bcbAlreadyTargetsBib = FragmentSet::ContainsFragmentEntirely(m_bcbBlockTypeTargets,
                FragmentSet::data_fragment_t(static_cast<std::size_t>(BPV7_BLOCK_TYPE_CODE::INTEGRITY),
                    static_cast<std::size_t>(BPV7_BLOCK_TYPE_CODE::INTEGRITY))); //just payload block
            if (bcbAlreadyTargetsBib) {
                LOG_DEBUG(subprocess) << "bpsec shall encrypt BIB since the BIB shares target(s) with the BCB";
            }
            else {
                LOG_FATAL(subprocess) << "bpsec policy must be fixed to encrypt the BIB since the BIB shares target(s) with the BCB";
                return false;
            }
        }
    }
    return true;
}

PolicySearchCache::PolicySearchCache() :
    securitySourceEid(0, 0),
    bundleSourceEid(0, 0),
    bundleFinalDestEid(0, 0),
    role(BPSEC_ROLE::RESERVED_MAX_ROLE_TYPES),
    wasCacheHit(false),
    foundPolicy(NULL) {}

BpSecPolicyProcessingContext::BpSecPolicyProcessingContext() :
    m_ivStruct(InitializationVectorsForOneThread::Create()),
    m_bcbTargetBibBlockNumberPlaceholderIndex(UINT64_MAX) {}

static BpSecPolicyFilter* Internal_AddPolicyFilterToThisFilter(const std::string& eidUri, BpSecPolicyFilter& thisPolicyFilter) {
    static const std::string anyUriString("ipn:*.*");
    BpSecPolicyFilter* newFilterPtr;

    if (eidUri == anyUriString) {
        if (!thisPolicyFilter.m_anyEidToNextFilterPtr) {
            thisPolicyFilter.m_anyEidToNextFilterPtr = boost::make_unique<BpSecPolicyFilter>();
        }
        newFilterPtr = thisPolicyFilter.m_anyEidToNextFilterPtr.get();
    }
    else {
        cbhe_eid_t destEid;
        bool serviceNumberIsWildCard;
        if (!Uri::ParseIpnUriString(eidUri, destEid.nodeId, destEid.serviceId, &serviceNumberIsWildCard)) {
            LOG_ERROR(subprocess) << "BpSecPolicyManager: eidUri " << eidUri << " is invalid.";
            return NULL;
        }
        if (serviceNumberIsWildCard) {
            newFilterPtr = &thisPolicyFilter.m_nodeIdToNextFilterMap[destEid.nodeId];
        }
        else { //fully qualified eid
            newFilterPtr = &thisPolicyFilter.m_eidToNextFilterMap[destEid];
        }
    }
    return newFilterPtr;
}
BpSecPolicy* BpSecPolicyManager::CreateOrGetNewPolicy(const std::string& securitySourceEidUri,
    const std::string& bundleSourceEidUri, const std::string& bundleFinalDestEidUri,
    const BPSEC_ROLE role, bool& isNewPolicy)
{
    isNewPolicy = true;
    BpSecPolicyFilter* policyFilterBundleSourcePtr = Internal_AddPolicyFilterToThisFilter(securitySourceEidUri, m_policyFilterSecuritySource);
    if (!policyFilterBundleSourcePtr) {
        return NULL;
    }
    BpSecPolicyFilter* policyFilterBundleFinalDestPtr = Internal_AddPolicyFilterToThisFilter(bundleSourceEidUri, *policyFilterBundleSourcePtr);
    if (!policyFilterBundleFinalDestPtr) {
        return NULL;
    }
    BpSecPolicyFilter* policyFilterRoleArraysPtr = Internal_AddPolicyFilterToThisFilter(bundleFinalDestEidUri, *policyFilterBundleFinalDestPtr);
    if (!policyFilterRoleArraysPtr) {
        return NULL;
    }
    const std::size_t roleIndex = static_cast<std::size_t>(role);
    if (roleIndex >= static_cast<std::size_t>(BPSEC_ROLE::RESERVED_MAX_ROLE_TYPES)) {
        return NULL;
    }
    BpSecPolicySharedPtr& policyPtr = policyFilterRoleArraysPtr->m_policiesByRoleArray[roleIndex];
    if (policyPtr) {
        //policy already exists
        isNewPolicy = false;
        return policyPtr.get(); 
    }
    policyPtr = std::make_shared<BpSecPolicy>();
    return policyPtr.get();
}
/*Policy rule lookup:
When a bundle is being received or generated, a policy rule must be found via the policy lookup function.

Lookup shall be performed by a Cascading lookup order:
        - The fully qualified [node,service] pair is looked up first for a match.
        - The node number only is looked up second for a match (for wildcard service numbers such as "ipn:2.*").
        - The "any destination flag" is looked up third for a match (for wildcard all such as "ipn:*.*").

The function shall take the following parameters:

1.) Security source:
    - "acceptor" or "verifier" role:
        - When a bundle is received, this field is the security source field of the ASB.
    - "source role":
        - When a bundle is received (i.e. being forwarded), this field is this receiving node's node number
        - When a new bundle is being created, this field is this bundle originator's node number
2.) Bundle source: The bundle source field of the primary block.
3.) Bundle final destination: The bundle destination field of the primary block.

"acceptor" or "verifier" should filter by security source field of the ASB
"source creators" should filter by their own node number
"source forwarders" should filter by their own node number and optionally bundle primary source and bundle primary dest

*/
static const BpSecPolicyFilter* Internal_GetPolicyFilterFromThisFilter(const cbhe_eid_t& eid, const BpSecPolicyFilter& thisPolicyFilter) {
    { //The fully qualified [node,service] pair is looked up first for a match.
        BpSecPolicyFilter::map_eid_to_next_filter_t::const_iterator it = thisPolicyFilter.m_eidToNextFilterMap.find(eid);
        if (it != thisPolicyFilter.m_eidToNextFilterMap.cend()) {
            return &(it->second);
        }
    }
    { //The node number only is looked up second for a match (for wildcard service numbers such as "ipn:2.*").
        BpSecPolicyFilter::map_node_id_to_next_filter_t::const_iterator it = thisPolicyFilter.m_nodeIdToNextFilterMap.find(eid.nodeId);
        if (it != thisPolicyFilter.m_nodeIdToNextFilterMap.cend()) {
            return &(it->second);
        }
    }
    //The "any destination flag" is looked up third for a match (for wildcard all such as "ipn:*.*").
    return thisPolicyFilter.m_anyEidToNextFilterPtr.get();

}
const BpSecPolicy* BpSecPolicyManager::FindPolicy(const cbhe_eid_t& securitySourceEid,
    const cbhe_eid_t& bundleSourceEid, const cbhe_eid_t& bundleFinalDestEid, const BPSEC_ROLE role) const
{
    const BpSecPolicyFilter* policyFilterBundleSourcePtr = Internal_GetPolicyFilterFromThisFilter(securitySourceEid, m_policyFilterSecuritySource);
    if (!policyFilterBundleSourcePtr) {
        return NULL;
    }
    const BpSecPolicyFilter* policyFilterBundleFinalDestPtr = Internal_GetPolicyFilterFromThisFilter(bundleSourceEid, *policyFilterBundleSourcePtr);
    if (!policyFilterBundleFinalDestPtr) {
        return NULL;
    }
    const BpSecPolicyFilter* policyFilterRoleArraysPtr = Internal_GetPolicyFilterFromThisFilter(bundleFinalDestEid, *policyFilterBundleFinalDestPtr);
    if (!policyFilterRoleArraysPtr) {
        return NULL;
    }
    const std::size_t roleIndex = static_cast<std::size_t>(role);
    if (roleIndex >= static_cast<std::size_t>(BPSEC_ROLE::RESERVED_MAX_ROLE_TYPES)) {
        return NULL;
    }
    return policyFilterRoleArraysPtr->m_policiesByRoleArray[roleIndex].get();
}

const BpSecPolicy* BpSecPolicyManager::FindPolicyWithCacheSupport(const cbhe_eid_t& securitySourceEid,
    const cbhe_eid_t& bundleSourceEid, const cbhe_eid_t& bundleFinalDestEid, const BPSEC_ROLE role, PolicySearchCache& searchCache) const
{
    searchCache.wasCacheHit = false;
    if ((role == searchCache.role)
        && (securitySourceEid == searchCache.securitySourceEid)
        && (bundleSourceEid == searchCache.bundleSourceEid)
        && (bundleFinalDestEid == searchCache.bundleFinalDestEid))
    {
        if (searchCache.foundPolicy) { //looked this up last time and succeeded
            searchCache.wasCacheHit = true;
            return searchCache.foundPolicy;
        }
        else { //attempted to look this up last time and failed
            return NULL;
        }
    }
    //never tried to look this up last time, look it up and cache the [failed or succeeded] result
    searchCache.foundPolicy = BpSecPolicyManager::FindPolicy(securitySourceEid,
        bundleSourceEid, bundleFinalDestEid, role);
    searchCache.role = role;
    searchCache.securitySourceEid = securitySourceEid;
    searchCache.bundleSourceEid = bundleSourceEid;
    searchCache.bundleFinalDestEid = bundleFinalDestEid;
    return searchCache.foundPolicy;
}


static bool DoesAsbTargetPayloadBlock(const Bpv7AbstractSecurityBlock* asbPtr) {
    for (Bpv7AbstractSecurityBlock::security_targets_t::const_iterator it = asbPtr->m_securityTargets.cbegin();
        it != asbPtr->m_securityTargets.cend(); ++it)
    {
        if (*it == 1) {
            return true;
        }
    }
    return false;
}

//keeping errors in a forward list acts as a stack and will result in the target/result index being in greatest to least order
static bool RemoveSopByGreatestToLeastIndex(BundleViewV7::Bpv7CanonicalBlockView& asbBlockView, Bpv7AbstractSecurityBlock* asbPtr, uint64_t i) {
    if (i == UINT64_MAX) { //special value denoting every index
        asbBlockView.markedForDeletion = true;
        return true;
    }
    if (asbPtr->m_securityTargets.size() != asbPtr->m_securityResults.size()) {
        return false;
    }
    if (i >= asbPtr->m_securityTargets.size()) {
        return false;
    }
    asbPtr->m_securityTargets.erase(asbPtr->m_securityTargets.begin() + i);
    asbPtr->m_securityResults.erase(asbPtr->m_securityResults.begin() + i);

    //5.1.1.  Receiving BCBs
    //When all security operations for a BCB have been removed from the BCB,
    //the BCB MUST be removed from the bundle.
    //5.1.2.  Receiving BIBs
    //When all security operations for a BIB have been removed from the BIB,
    //the BIB MUST be removed from the bundle.
    if (asbPtr->m_securityTargets.empty()) {
        
        asbBlockView.markedForDeletion = true;
    }

    asbBlockView.SetManuallyModified();
    return true;
}


static void RemoveSopTargetBlock(BundleViewV7& bv, BundleViewV7::Bpv7CanonicalBlockView& asbBlockView,
    Bpv7AbstractSecurityBlock* asbPtr, uint64_t canonicalIndex)
{
    (void)asbBlockView;
    if (canonicalIndex == UINT64_MAX) { //special value denoting every block targeted by ASB
        for (Bpv7AbstractSecurityBlock::security_targets_t::const_iterator it = asbPtr->m_securityTargets.cbegin();
            it != asbPtr->m_securityTargets.cend(); ++it)
        {
            if (BundleViewV7::Bpv7CanonicalBlockView* view = bv.GetCanonicalBlockByBlockNumber(*it)) {
                view->markedForDeletion = true;
            }
        }
    }
    else {
        if (BundleViewV7::Bpv7CanonicalBlockView* view = bv.GetCanonicalBlockByBlockNumber(canonicalIndex)) {
            view->markedForDeletion = true;
        }
    }
}

static bool DoFailureEventSopMissingAtAcceptor(BundleViewV7& bv,
    BPSEC_SECURITY_FAILURE_PROCESSING_ACTION_MASKS actionMaskSopMissingAtAcceptor,
    BundleViewV7::Bpv7CanonicalBlockView& asbBlockView, Bpv7AbstractSecurityBlock* asbPtr, const bool isIntegrity)
{
    const char* securityServiceStr = (isIntegrity) ? "integrity" : "confidentiality";
    typedef BPSEC_SECURITY_FAILURE_PROCESSING_ACTION_MASKS action_mask_t;
    const bool asbTargetsPayloadBlock = DoesAsbTargetPayloadBlock(asbPtr);

    //acceptor
    //5.1.1.  Receiving BCBs
    //If an encrypted payload block cannot be decrypted (i.e., the
    //ciphertext cannot be authenticated), then the bundle MUST be
    //discarded and processed no further.
    if ((!isIntegrity) && asbTargetsPayloadBlock) {
        static thread_local bool printedMsg = false;
        if (!printedMsg) {
            LOG_WARNING(subprocess) << "first time encrypted payload block cannot be decrypted (SopMissingAtAcceptor) from source node "
                << bv.m_primaryBlockView.header.m_sourceNodeId
                << ".. bundle shall be dropped..(This message type will now be suppressed.)";
            printedMsg = true;
        }
        return false; //drop bundle
    }

    //prohibited action: remove SOp (checked by JSON config file)
    //asbBlockView.markedForDeletion = true;


    static const char* evtString = "sopMissingAtAcceptor";
    const action_mask_t actionMask = actionMaskSopMissingAtAcceptor;
    if ((actionMask & action_mask_t::FAIL_BUNDLE_FORWARDING) != action_mask_t::NO_ACTIONS_SET) { //optional action
        static thread_local bool printedMsg = false;
        if (!printedMsg) {
            LOG_WARNING(subprocess) << "first time " << evtString << " from source node "
                << bv.m_primaryBlockView.header.m_sourceNodeId
                << ".. FAIL_BUNDLE_FORWARDING specified (bundle shall be dropped)..(This message type will now be suppressed.)";
            printedMsg = true;
        }
        return false; //drop bundle
    }
    else if ((actionMask & action_mask_t::REMOVE_SECURITY_OPERATION_TARGET_BLOCK) != action_mask_t::NO_ACTIONS_SET) { //optional action
        if (asbTargetsPayloadBlock) {
            static thread_local bool printedMsg = false;
            if (!printedMsg) {
                LOG_WARNING(subprocess) << "first time " << evtString << " from source node "
                    << bv.m_primaryBlockView.header.m_sourceNodeId
                    << ".. REMOVE_SECURITY_OPERATION_TARGET_BLOCK specified but target(s) includes payload block, (bundle shall be dropped)..(This message type will now be suppressed.)";
                printedMsg = true;
            }
            return false; //drop bundle
        }
        else {
            static thread_local bool printedMsg = false;
            if (!printedMsg) {
                LOG_WARNING(subprocess) << "first time " << evtString << " from source node "
                    << bv.m_primaryBlockView.header.m_sourceNodeId
                    << ".. REMOVE_SECURITY_OPERATION_TARGET_BLOCK specified ..(This message type will now be suppressed.)";
                printedMsg = true;
            }
            RemoveSopTargetBlock(bv, asbBlockView, asbPtr, UINT64_MAX);
        }
    }
    else {
        LOG_WARNING(subprocess) << "Process: version 7 bundle received but cannot accept " << securityServiceStr << " (no failure actions taken)";
    }

    return true;
}

static bool DoFailureEvent(BundleViewV7& bv, const BpSecPolicy* bpSecPolicyPtr, const BpSecBundleProcessor::BpSecErrorFlist& errorList,
    BundleViewV7::Bpv7CanonicalBlockView& asbBlockView, Bpv7AbstractSecurityBlock* asbPtr, const bool isAcceptor, const bool isIntegrity)
{
    const event_type_to_event_set_ptr_lut_t& evtLut = (isIntegrity) ?
        bpSecPolicyPtr->m_integritySecurityFailureEventSetReferencePtr->m_eventTypeToEventSetPtrLut :
        bpSecPolicyPtr->m_confidentialitySecurityFailureEventSetReferencePtr->m_eventTypeToEventSetPtrLut;
    const char* securityServiceStr = (isIntegrity) ? "integrity" : "confidentiality";
    typedef BPSEC_SECURITY_FAILURE_PROCESSING_ACTION_MASKS action_mask_t;

    for (BpSecBundleProcessor::BpSecErrorFlist::const_iterator itErr = errorList.cbegin();
        itErr != errorList.cend(); ++itErr)
    {
        const BpSecBundleProcessor::BpSecError& thisError = *itErr;
        const uint64_t canonicalIndex = (thisError.m_securityTargetIndex == UINT64_MAX) ?
            UINT64_MAX : asbPtr->m_securityTargets[thisError.m_securityTargetIndex];
        const bool errorTargetsPayloadBlock = (canonicalIndex == 1) || (canonicalIndex == UINT64_MAX);
        bool removedSop = false;
        bool removedSopTarget = false;
        if (isAcceptor) { //acceptor
            //5.1.1.  Receiving BCBs
            if (!isIntegrity) { //this is a BCB
                //If an encrypted payload block cannot be decrypted (i.e., the
                //ciphertext cannot be authenticated), then the bundle MUST be
                //discarded and processed no further.
                if (errorTargetsPayloadBlock) {
                    static thread_local bool printedMsg = false;
                        if (!printedMsg) {
                            LOG_WARNING(subprocess) << "first time encrypted payload block cannot be decrypted by acceptor from source node "
                                << bv.m_primaryBlockView.header.m_sourceNodeId
                                << ".. bundle shall be dropped..(This message type will now be suppressed.)";
                                printedMsg = true;
                        }
                    return false; //drop bundle
                }
                //If an encrypted security target other than the payload block cannot be decrypted,
                //then the associated security target and all security blocks associated with that target
                //MUST be discarded and processed no further.
                else {
                    if (!RemoveSopByGreatestToLeastIndex(asbBlockView, asbPtr, thisError.m_securityTargetIndex)) {
                        LOG_ERROR(subprocess) << "unexpected acceptor error in RemoveSopByGreatestToLeastIndex of securityTargetIndex "
                            << thisError.m_securityTargetIndex << " ..dropping bundle";
                        return false; //drop bundle
                    }
                    RemoveSopTargetBlock(bv, asbBlockView, asbPtr, canonicalIndex);
                    removedSop = true;
                    removedSopTarget = true;
                    //must continue down to check for FAIL_BUNDLE_FORWARDING
                }
            }

            BPSEC_SECURITY_FAILURE_EVENT evt = BPSEC_SECURITY_FAILURE_EVENT::UNDEFINED;
            const char* evtString = "";
            if (thisError.m_errorCode == BpSecBundleProcessor::BPSEC_ERROR_CODES::CORRUPTED) {
                evt = BPSEC_SECURITY_FAILURE_EVENT::SECURITY_OPERATION_CORRUPTED_AT_ACCEPTOR;
                evtString = "SECURITY_OPERATION_CORRUPTED_AT_ACCEPTOR";
            }
            else if (thisError.m_errorCode == BpSecBundleProcessor::BPSEC_ERROR_CODES::MISCONFIGURED) {
                evt = BPSEC_SECURITY_FAILURE_EVENT::SECURITY_OPERATION_MISCONFIGURED_AT_ACCEPTOR;
                evtString = "SECURITY_OPERATION_MISCONFIGURED_AT_ACCEPTOR";
            }
            if (evtLut[static_cast<uint8_t>(evt)]) {
                //required actions: remove SOp
                if (!removedSop) {
                    if (!RemoveSopByGreatestToLeastIndex(asbBlockView, asbPtr, thisError.m_securityTargetIndex)) {
                        LOG_ERROR(subprocess) << "unexpected acceptor error in RemoveSopByGreatestToLeastIndex of securityTargetIndex "
                            << thisError.m_securityTargetIndex << " ..dropping bundle";
                        return false; //drop bundle
                    }
                    removedSop = true;
                }

                action_mask_t actionMask = evtLut[static_cast<uint8_t>(evt)]->m_actionMasks;
                if ((actionMask & action_mask_t::FAIL_BUNDLE_FORWARDING) != action_mask_t::NO_ACTIONS_SET) { //optional action
                    static thread_local bool printedMsg = false;
                    if (!printedMsg) {
                        LOG_WARNING(subprocess) << "first time " << evtString << " from source node "
                            << bv.m_primaryBlockView.header.m_sourceNodeId
                            << ".. FAIL_BUNDLE_FORWARDING specified (bundle shall be dropped)..(This message type will now be suppressed.)";
                        printedMsg = true;
                    }
                    return false; //drop bundle
                }
                else if ((actionMask & action_mask_t::REMOVE_SECURITY_OPERATION_TARGET_BLOCK) != action_mask_t::NO_ACTIONS_SET) { //optional action
                    if (errorTargetsPayloadBlock) {
                        static thread_local bool printedMsg = false;
                        if (!printedMsg) {
                            LOG_WARNING(subprocess) << "first time " << evtString << " from source node "
                                << bv.m_primaryBlockView.header.m_sourceNodeId
                                << ".. REMOVE_SECURITY_OPERATION_TARGET_BLOCK specified but target(s) includes payload block, (bundle shall be dropped)..(This message type will now be suppressed.)";
                            printedMsg = true;
                        }
                        return false; //drop bundle
                    }
                    else {
                        static thread_local bool printedMsg = false;
                        if (!printedMsg) {
                            LOG_WARNING(subprocess) << "first time " << evtString << " from source node "
                                << bv.m_primaryBlockView.header.m_sourceNodeId
                                << ".. REMOVE_SECURITY_OPERATION_TARGET_BLOCK specified ..(This message type will now be suppressed.)";
                            printedMsg = true;
                        }
                        if (!removedSopTarget) {
                            RemoveSopTargetBlock(bv, asbBlockView, asbPtr, canonicalIndex);
                            removedSopTarget = true;
                        }
                    }
                }
                else {
                    LOG_WARNING(subprocess) << "Process: version 7 bundle received but cannot accept " << securityServiceStr << " (no failure actions taken)";
                }
            }
            else {
                LOG_ERROR(subprocess) << "Process: version 7 bundle received but cannot accept " << securityServiceStr << " (no failure events specified)";
                return false;
            }
        }
        else { //verifier
            //SOp corrupted and SOp misconfigured
            BPSEC_SECURITY_FAILURE_EVENT evt = BPSEC_SECURITY_FAILURE_EVENT::UNDEFINED;
            const char* evtString = "";
            if (thisError.m_errorCode == BpSecBundleProcessor::BPSEC_ERROR_CODES::CORRUPTED) {
                evt = BPSEC_SECURITY_FAILURE_EVENT::SECURITY_OPERATION_CORRUPTED_AT_VERIFIER;
                evtString = "SECURITY_OPERATION_CORRUPTED_AT_VERIFIER";
            }
            else if (thisError.m_errorCode == BpSecBundleProcessor::BPSEC_ERROR_CODES::MISCONFIGURED) {
                evt = BPSEC_SECURITY_FAILURE_EVENT::SECURITY_OPERATION_MISCONFIGURED_AT_VERIFIER;
                evtString = "SECURITY_OPERATION_MISCONFIGURED_AT_VERIFIER";
            }
            if (evtLut[static_cast<uint8_t>(evt)]) {

                action_mask_t actionMask = evtLut[static_cast<uint8_t>(evt)]->m_actionMasks;
                bool tookAction = false;
                if ((actionMask & action_mask_t::REMOVE_SECURITY_OPERATION) != action_mask_t::NO_ACTIONS_SET) { //optional action
                    static thread_local bool printedMsg = false;
                    if (!printedMsg) {
                        LOG_WARNING(subprocess) << "first time " << evtString << " from source node "
                            << bv.m_primaryBlockView.header.m_sourceNodeId
                            << ".. REMOVE_SECURITY_OPERATION specified..(This message type will now be suppressed.)";
                        printedMsg = true;
                    }
                    if (!RemoveSopByGreatestToLeastIndex(asbBlockView, asbPtr, thisError.m_securityTargetIndex)) {
                        LOG_ERROR(subprocess) << "unexpected verifier error in RemoveSopByGreatestToLeastIndex of securityTargetIndex "
                            << thisError.m_securityTargetIndex << " ..dropping bundle";
                        return false; //drop bundle
                    }
                    tookAction = true;
                }
                if ((actionMask & action_mask_t::FAIL_BUNDLE_FORWARDING) != action_mask_t::NO_ACTIONS_SET) { //optional action
                    static thread_local bool printedMsg = false;
                    if (!printedMsg) {
                        LOG_WARNING(subprocess) << "first time " << evtString << " from source node "
                            << bv.m_primaryBlockView.header.m_sourceNodeId
                            << ".. FAIL_BUNDLE_FORWARDING specified (bundle shall be dropped)..(This message type will now be suppressed.)";
                        printedMsg = true;
                    }
                    return false; //drop bundle
                }
                else if ((actionMask & action_mask_t::REMOVE_SECURITY_OPERATION_TARGET_BLOCK) != action_mask_t::NO_ACTIONS_SET) { //optional action
                    tookAction = true;
                    if (errorTargetsPayloadBlock) {
                        static thread_local bool printedMsg = false;
                        if (!printedMsg) {
                            LOG_WARNING(subprocess) << "first time " << evtString << " from source node "
                                << bv.m_primaryBlockView.header.m_sourceNodeId
                                << ".. REMOVE_SECURITY_OPERATION_TARGET_BLOCK specified but target(s) includes payload block, (bundle shall be dropped)..(This message type will now be suppressed.)";
                            printedMsg = true;
                        }
                        return false; //drop bundle
                    }
                    else {
                        static thread_local bool printedMsg = false;
                        if (!printedMsg) {
                            LOG_WARNING(subprocess) << "first time " << evtString << " from source node "
                                << bv.m_primaryBlockView.header.m_sourceNodeId
                                << ".. REMOVE_SECURITY_OPERATION_TARGET_BLOCK specified ..(This message type will now be suppressed.)";
                            printedMsg = true;
                        }
                        RemoveSopTargetBlock(bv, asbBlockView, asbPtr, canonicalIndex);
                    }
                }
                if (!tookAction) {
                    LOG_WARNING(subprocess) << "Process: version 7 bundle received but cannot do security operation (no failure actions taken)";
                }
            }
            else {
                LOG_ERROR(subprocess) << "Process: version 7 bundle received but cannot do security operation (no failure events specified)";
                return false;
            }
        }
    }
    return true;
}

bool BpSecPolicyManager::ProcessReceivedBundle(BundleViewV7& bv, BpSecPolicyProcessingContext& ctx,
    BpSecBundleProcessor::BpSecErrorFlist& errorList, const uint64_t myNodeId) const
{
    //ignorning the following flag: bool hadError = false;
    const Bpv7CbhePrimaryBlock& primary = bv.m_primaryBlockView.header;
    const bool bundleIsAtFinalDest = (primary.m_destinationEid.nodeId == myNodeId);
    bv.GetCanonicalBlocksByType(BPV7_BLOCK_TYPE_CODE::CONFIDENTIALITY, ctx.m_tmpBlocks);
    for (std::size_t i = 0; i < ctx.m_tmpBlocks.size(); ++i) {
        BundleViewV7::Bpv7CanonicalBlockView& bcbBlockView = *(ctx.m_tmpBlocks[i]);
        Bpv7BlockConfidentialityBlock* bcbPtr = dynamic_cast<Bpv7BlockConfidentialityBlock*>(bcbBlockView.headerPtr.get());
        if (!bcbPtr) {
            LOG_ERROR(subprocess) << "cast to bcb block failed";
            return false;
        }
        const BpSecPolicy* bpSecPolicyPtr = FindPolicyWithCacheSupport(
            bcbPtr->m_securitySource, primary.m_sourceNodeId, primary.m_destinationEid, BPSEC_ROLE::ACCEPTOR, ctx.m_searchCacheBcbAcceptor);
        bool verifyOnly;
        if (bpSecPolicyPtr) {
            verifyOnly = false; //false for acceptors
        }
        else if (bundleIsAtFinalDest) {
            const std::string asbInfoStr = std::string("securitySource=")
                + Uri::GetIpnUriString(bcbPtr->m_securitySource.nodeId, bcbPtr->m_securitySource.serviceId)
                + ",bundleSource=" + Uri::GetIpnUriString(primary.m_sourceNodeId.nodeId, primary.m_sourceNodeId.serviceId)
                + ",bundleFinalDest=" + Uri::GetIpnUriString(primary.m_destinationEid.nodeId, primary.m_destinationEid.serviceId);
            errorList.emplace_front(BpSecBundleProcessor::BPSEC_ERROR_CODES::MISSING, UINT64_MAX, boost::make_unique<std::string>(
                std::string("Bundle is at final destination but an acceptor policy could not be found for BCB with ")
                + asbInfoStr)); //null for some reason

            bool dontDropBundle = DoFailureEventSopMissingAtAcceptor(bv, m_actionMaskSopMissingAtAcceptor,
                bcbBlockView, bcbPtr, false);
            if (!dontDropBundle) {
                return false; //drop bundle
            }
            else {
                continue;
            }
        }
        else {
            bpSecPolicyPtr = FindPolicyWithCacheSupport(
                bcbPtr->m_securitySource, primary.m_sourceNodeId, primary.m_destinationEid, BPSEC_ROLE::VERIFIER, ctx.m_searchCacheBcbVerifier);
            if (!bpSecPolicyPtr) {
                continue;
            }
            verifyOnly = true; //true for verifiers
        }
        if (!bpSecPolicyPtr->m_doConfidentiality) {
            continue;
        }

        BpSecBundleProcessor::ConfidentialityReceivedParameters crp;
        crp.keyEncryptionKey = bpSecPolicyPtr->m_confidentialityKeyEncryptionKey.empty() ?
            NULL : bpSecPolicyPtr->m_confidentialityKeyEncryptionKey.data(), //NULL if not present (for wrapping DEK only)
        crp.keyEncryptionKeyLength = static_cast<unsigned int>(bpSecPolicyPtr->m_confidentialityKeyEncryptionKey.size());
        crp.dataEncryptionKey = bpSecPolicyPtr->m_dataEncryptionKey.empty() ?
            NULL : bpSecPolicyPtr->m_dataEncryptionKey.data(); //NULL if not present (when no wrapped key is present)
        crp.dataEncryptionKeyLength = static_cast<unsigned int>(bpSecPolicyPtr->m_dataEncryptionKey.size());
        crp.expectedIvLength = bpSecPolicyPtr->m_use12ByteIv ? 12 : 16;
        crp.expectedVariant = bpSecPolicyPtr->m_confidentialityVariant;
        crp.expectedAadScopeMask = bpSecPolicyPtr->m_aadScopeMask;
        crp.expectedTargetBlockTypesMask = 0;
        for (FragmentSet::data_fragment_set_t::const_iterator it = bpSecPolicyPtr->m_bcbBlockTypeTargets.cbegin();
            it != bpSecPolicyPtr->m_bcbBlockTypeTargets.cend(); ++it)
        {
            const FragmentSet::data_fragment_t& df = *it;
            for (uint64_t blockType = df.beginIndex; blockType <= df.endIndex; ++blockType) {
                crp.expectedTargetBlockTypesMask |= ((uint64_t)(1)) << blockType;
            }
        }

        //does not rerender in place here, there are more ops to complete after decryption and then a manual render-in-place will be called later
        errorList = BpSecBundleProcessor::TryDecryptBundleByIndividualBcb(ctx.m_evpCtxWrapper,
            ctx.m_ctxWrapperKeyWrapOps,
            bv,
            bcbBlockView,
            crp,
            ctx.m_bpsecReusableElementsInternal,
            verifyOnly);
        if (!errorList.empty()) {
            //an error occurred: hadError = true;
            bool dontDropBundle = DoFailureEvent(bv, bpSecPolicyPtr, errorList,
                bcbBlockView, bcbPtr, !verifyOnly, false);

            static thread_local bool printedMsg = false;
            if (!printedMsg) {
                LOG_WARNING(subprocess) << "first time version 7 bundle received but cannot decrypt..(This message type will now be suppressed.)";
                printedMsg = true;
            }
            if (!dontDropBundle) {
                return false; //drop bundle
            }
        }
        else {
            //all decryption ops successful for this bcb block at this point
            if (verifyOnly) {
                static thread_local bool printedMsg = false;
                if (!printedMsg) {
                    LOG_INFO(subprocess) << "first time VERIFIED THE DECRYPTION of a bundle successfully from source node "
                        << bv.m_primaryBlockView.header.m_sourceNodeId
                        << " ..(This message type will now be suppressed.)";
                    printedMsg = true;
                }
            }
            else {
                static thread_local bool printedMsg = false;
                if (!printedMsg) {
                    LOG_INFO(subprocess) << "first time ACCEPTED/DECRYPTED a bundle successfully from source node "
                        << bv.m_primaryBlockView.header.m_sourceNodeId
                        << " ..(This message type will now be suppressed.)";
                    printedMsg = true;
                }
            }
        }
    }

    

    
    bv.GetCanonicalBlocksByType(BPV7_BLOCK_TYPE_CODE::INTEGRITY, ctx.m_tmpBlocks);
    for (std::size_t i = 0; i < ctx.m_tmpBlocks.size(); ++i) {
        BundleViewV7::Bpv7CanonicalBlockView& bibBlockView = *(ctx.m_tmpBlocks[i]);
        Bpv7BlockIntegrityBlock* bibPtr = dynamic_cast<Bpv7BlockIntegrityBlock*>(bibBlockView.headerPtr.get());
        if (!bibPtr) {
            LOG_ERROR(subprocess) << "cast to bib block failed";
            return false;
        }
        const BpSecPolicy* bpSecPolicyPtr = FindPolicyWithCacheSupport(
            bibPtr->m_securitySource, primary.m_sourceNodeId, primary.m_destinationEid, BPSEC_ROLE::ACCEPTOR, ctx.m_searchCacheBibAcceptor);
        bool markBibForDeletion;
        if (bpSecPolicyPtr) {
            markBibForDeletion = true; //true for acceptors
        }
        else if (bundleIsAtFinalDest) {
            const std::string asbInfoStr = std::string("securitySource=")
                + Uri::GetIpnUriString(bibPtr->m_securitySource.nodeId, bibPtr->m_securitySource.serviceId)
                + ",bundleSource=" + Uri::GetIpnUriString(primary.m_sourceNodeId.nodeId, primary.m_sourceNodeId.serviceId)
                + ",bundleFinalDest=" + Uri::GetIpnUriString(primary.m_destinationEid.nodeId, primary.m_destinationEid.serviceId);
            errorList.emplace_front(BpSecBundleProcessor::BPSEC_ERROR_CODES::MISSING, UINT64_MAX, boost::make_unique<std::string>(
                std::string("Bundle is at final destination but an acceptor policy could not be found for BIB with ")
                + asbInfoStr)); //null for some reason
            bool dontDropBundle = DoFailureEventSopMissingAtAcceptor(bv, m_actionMaskSopMissingAtAcceptor,
                bibBlockView, bibPtr, true);
            if (!dontDropBundle) {
                return false; //drop bundle
            }
            else {
                continue;
            }
        }
        else {
            bpSecPolicyPtr = FindPolicyWithCacheSupport(
                bibPtr->m_securitySource, primary.m_sourceNodeId, primary.m_destinationEid, BPSEC_ROLE::VERIFIER, ctx.m_searchCacheBibVerifier);
            if (!bpSecPolicyPtr) {
                continue;
            }
            markBibForDeletion = false; //false for verifiers
        }

        if (!bpSecPolicyPtr->m_doIntegrity) {
            continue;
        }

        BpSecBundleProcessor::IntegrityReceivedParameters irp;
        irp.keyEncryptionKey = bpSecPolicyPtr->m_hmacKeyEncryptionKey.empty() ?
            NULL : bpSecPolicyPtr->m_hmacKeyEncryptionKey.data(); //NULL if not present (for unwrapping hmac key only)
        irp.keyEncryptionKeyLength = static_cast<unsigned int>(bpSecPolicyPtr->m_hmacKeyEncryptionKey.size());
        irp.hmacKey = bpSecPolicyPtr->m_hmacKey.empty() ?
            NULL : bpSecPolicyPtr->m_hmacKey.data(), //NULL if not present (when no wrapped key is present)
            irp.hmacKeyLength = static_cast<unsigned int>(bpSecPolicyPtr->m_hmacKey.size());
        irp.expectedVariant = bpSecPolicyPtr->m_integrityVariant;
        irp.expectedScopeMask = bpSecPolicyPtr->m_integrityScopeMask;
        irp.expectedTargetBlockTypesMask = 0;
        for (FragmentSet::data_fragment_set_t::const_iterator it = bpSecPolicyPtr->m_bibBlockTypeTargets.cbegin();
            it != bpSecPolicyPtr->m_bibBlockTypeTargets.cend(); ++it)
        {
            const FragmentSet::data_fragment_t& df = *it;
            for (uint64_t blockType = df.beginIndex; blockType <= df.endIndex; ++blockType) {
                irp.expectedTargetBlockTypesMask |= ((uint64_t)(1)) << blockType;
            }
        }
        //does not rerender in place here, there are more ops to complete after decryption and then a manual render-in-place will be called later
        errorList = BpSecBundleProcessor::TryVerifyBundleIntegrityByIndividualBib(ctx.m_hmacCtxWrapper,
            ctx.m_ctxWrapperKeyWrapOps,
            bv,
            bibBlockView,
            irp,
            ctx.m_bpsecReusableElementsInternal,
            markBibForDeletion);
        if (!errorList.empty()) {
            //an error occurred: hadError = true;
            bool dontDropBundle = DoFailureEvent(bv, bpSecPolicyPtr, errorList, bibBlockView, bibPtr, markBibForDeletion, true);

            static thread_local bool printedMsg = false;
            if (!printedMsg) {
                LOG_WARNING(subprocess) << "first time version 7 bundle received but cannot check integrity..(This message type will now be suppressed.)";
                printedMsg = true;
            }
            if (!dontDropBundle) {
                return false; //drop bundle
            }
        }
        else {
            //all verification ops successful for this bib block at this point
            if (markBibForDeletion) {
                static thread_local bool printedMsg = false;
                if (!printedMsg) {
                    LOG_INFO(subprocess) << "first time ACCEPTED a bundle's integrity successfully from source node "
                        << bv.m_primaryBlockView.header.m_sourceNodeId
                        << " ..(This message type will now be suppressed.)";
                    printedMsg = true;
                }
            }
            else {
                static thread_local bool printedMsg = false;
                if (!printedMsg) {
                    LOG_INFO(subprocess) << "first time VERIFIED a bundle's integrity successfully from source node "
                        << bv.m_primaryBlockView.header.m_sourceNodeId
                        << " ..(This message type will now be suppressed.)";
                    printedMsg = true;
                }
            }
        }
    }
    
    return true;
}

bool BpSecPolicyManager::PopulateTargetArraysForSecuritySource(BundleViewV7& bv,
    BpSecPolicyProcessingContext& ctx,
    const BpSecPolicy& policy)
{
    ctx.m_bibTargetBlockNumbers.clear();
    ctx.m_bcbTargetBlockNumbers.clear();
    ctx.m_bcbTargetBibBlockNumberPlaceholderIndex = UINT64_MAX;
    if (policy.m_doIntegrity) {
        static thread_local bool printedMsg = false;
        for (FragmentSet::data_fragment_set_t::const_iterator it = policy.m_bibBlockTypeTargets.cbegin();
            it != policy.m_bibBlockTypeTargets.cend(); ++it)
        {
            const FragmentSet::data_fragment_t& df = *it;
            for (uint64_t blockType = df.beginIndex; blockType <= df.endIndex; ++blockType) {
                bv.GetCanonicalBlocksByType(static_cast<BPV7_BLOCK_TYPE_CODE>(blockType), ctx.m_tmpBlocks);
                
                for (std::size_t i = 0; i < ctx.m_tmpBlocks.size(); ++i) {
                    ctx.m_bibTargetBlockNumbers.push_back(ctx.m_tmpBlocks[i]->headerPtr->m_blockNumber);
                    if (!printedMsg) {
                        LOG_DEBUG(subprocess) << "first time bpsec security source adds integrity target for block number "
                            << ctx.m_bibTargetBlockNumbers.back()
                            << " ..(This message type will now be suppressed.)";
                    }
                }
            }
        }
        printedMsg = true;
    }
    if (policy.m_doConfidentiality) {
        static thread_local bool printedMsg = false;
        for (FragmentSet::data_fragment_set_t::const_iterator it = policy.m_bcbBlockTypeTargets.cbegin();
            it != policy.m_bcbBlockTypeTargets.cend(); ++it)
        {
            const FragmentSet::data_fragment_t& df = *it;
            for (uint64_t blockType = df.beginIndex; blockType <= df.endIndex; ++blockType) {
                if (blockType == static_cast<uint64_t>(BPV7_BLOCK_TYPE_CODE::INTEGRITY)) {
                    //integrity block number is auto-assigned later
                    ctx.m_bcbTargetBibBlockNumberPlaceholderIndex = ctx.m_bcbTargetBlockNumbers.size();
                    ctx.m_bcbTargetBlockNumbers.push_back(0);
                    if (!printedMsg) {
                        LOG_DEBUG(subprocess) << "first time bpsec add block target confidentiality placeholder for bib ..(This message type will now be suppressed.)";
                    }
                }
                else {
                    bv.GetCanonicalBlocksByType(static_cast<BPV7_BLOCK_TYPE_CODE>(blockType), ctx.m_tmpBlocks);
                    for (std::size_t i = 0; i < ctx.m_tmpBlocks.size(); ++i) {
                        ctx.m_bcbTargetBlockNumbers.push_back(ctx.m_tmpBlocks[i]->headerPtr->m_blockNumber);
                        if (!printedMsg) {
                            LOG_DEBUG(subprocess) << "first time bpsec security source adds confidentiality target for block number "
                                << ctx.m_bcbTargetBlockNumbers.back()
                                << " ..(This message type will now be suppressed.)";
                        }
                    }
                }
            }
        }
        printedMsg = true;
    }
    return true;
}

bool BpSecPolicyManager::PopulateTargetArraysForSecuritySource(
    const uint8_t* bpv7BlockTypeToManuallyAssignedBlockNumberLut,
    BpSecPolicyProcessingContext& ctx,
    const BpSecPolicy& policy)
{
    static constexpr std::size_t MAX_NUM_BPV7_BLOCK_TYPE_CODES = static_cast<std::size_t>(BPV7_BLOCK_TYPE_CODE::RESERVED_MAX_BLOCK_TYPES);
    ctx.m_bibTargetBlockNumbers.clear();
    ctx.m_bcbTargetBlockNumbers.clear();
    ctx.m_bcbTargetBibBlockNumberPlaceholderIndex = UINT64_MAX;
    if (policy.m_doIntegrity) {
        for (FragmentSet::data_fragment_set_t::const_iterator it = policy.m_bibBlockTypeTargets.cbegin();
            it != policy.m_bibBlockTypeTargets.cend(); ++it)
        {
            const FragmentSet::data_fragment_t& df = *it;
            for (uint64_t blockType = df.beginIndex; blockType <= df.endIndex; ++blockType) {
                if (blockType >= MAX_NUM_BPV7_BLOCK_TYPE_CODES) {
                    LOG_FATAL(subprocess) << "policy error: invalid block type " << blockType;
                    return false;
                }
                ctx.m_bibTargetBlockNumbers.push_back(bpv7BlockTypeToManuallyAssignedBlockNumberLut[blockType]);
                LOG_DEBUG(subprocess) << "bpsec add block target integrity " << ctx.m_bibTargetBlockNumbers.back();
            }
        }
    }
    if (policy.m_doConfidentiality) {
        for (FragmentSet::data_fragment_set_t::const_iterator it = policy.m_bcbBlockTypeTargets.cbegin();
            it != policy.m_bcbBlockTypeTargets.cend(); ++it)
        {
            const FragmentSet::data_fragment_t& df = *it;
            for (uint64_t blockType = df.beginIndex; blockType <= df.endIndex; ++blockType) {
                if (blockType >= MAX_NUM_BPV7_BLOCK_TYPE_CODES) {
                    LOG_FATAL(subprocess) << "policy error: invalid block type " << blockType;
                    return false;
                }
                if (blockType == static_cast<uint64_t>(BPV7_BLOCK_TYPE_CODE::INTEGRITY)) {
                    //integrity block number is auto-assigned later
                    ctx.m_bcbTargetBibBlockNumberPlaceholderIndex = ctx.m_bcbTargetBlockNumbers.size();
                    ctx.m_bcbTargetBlockNumbers.push_back(0);
                    LOG_DEBUG(subprocess) << "bpsec add block target confidentiality placeholder for bib";
                }
                else {
                    ctx.m_bcbTargetBlockNumbers.push_back(bpv7BlockTypeToManuallyAssignedBlockNumberLut[blockType]);
                    LOG_DEBUG(subprocess) << "bpsec add block target confidentiality " << ctx.m_bcbTargetBlockNumbers.back();
                }
            }
        }
    }
    return true;
}

bool BpSecPolicyManager::ProcessOutgoingBundle(BundleViewV7& bv,
    BpSecPolicyProcessingContext& ctx,
    const BpSecPolicy& policy,
    const cbhe_eid_t& thisSecuritySourceEid)
{
    if (policy.m_doIntegrity) {
        if (!BpSecBundleProcessor::TryAddBundleIntegrity(ctx.m_hmacCtxWrapper,
            ctx.m_ctxWrapperKeyWrapOps,
            bv,
            policy.m_integrityScopeMask,
            policy.m_integrityVariant,
            policy.m_bibCrcType,
            thisSecuritySourceEid,
            ctx.m_bibTargetBlockNumbers.data(), static_cast<unsigned int>(ctx.m_bibTargetBlockNumbers.size()),
            policy.m_hmacKeyEncryptionKey.empty() ? NULL : policy.m_hmacKeyEncryptionKey.data(), //NULL if not present (for unwrapping hmac key only)
            static_cast<unsigned int>(policy.m_hmacKeyEncryptionKey.size()),
            policy.m_hmacKey.empty() ? NULL : policy.m_hmacKey.data(), //NULL if not present (when no wrapped key is present)
            static_cast<unsigned int>(policy.m_hmacKey.size()),
            ctx.m_bpsecReusableElementsInternal,
            NULL, //bib placed immediately after primary
            true))
        {
            LOG_ERROR(subprocess) << "cannot add integrity to bundle";
            return false;
        }
        if (ctx.m_bcbTargetBibBlockNumberPlaceholderIndex != UINT64_MAX) {
            ctx.m_bcbTargetBlockNumbers[ctx.m_bcbTargetBibBlockNumberPlaceholderIndex] = bv.m_listCanonicalBlockView.front().headerPtr->m_blockNumber;
        }
    }
    if (policy.m_doConfidentiality) {
        ctx.m_ivStruct.SerializeAndIncrement(policy.m_use12ByteIv);
        if (!BpSecBundleProcessor::TryEncryptBundle(ctx.m_evpCtxWrapper,
            ctx.m_ctxWrapperKeyWrapOps,
            bv,
            policy.m_aadScopeMask,
            policy.m_confidentialityVariant,
            policy.m_bcbCrcType,
            thisSecuritySourceEid,
            ctx.m_bcbTargetBlockNumbers.data(), static_cast<unsigned int>(ctx.m_bcbTargetBlockNumbers.size()),
            ctx.m_ivStruct.m_initializationVector.data(), static_cast<unsigned int>(ctx.m_ivStruct.m_initializationVector.size()),
            policy.m_confidentialityKeyEncryptionKey.empty() ? NULL : policy.m_confidentialityKeyEncryptionKey.data(), //NULL if not present (for wrapping DEK only)
            static_cast<unsigned int>(policy.m_confidentialityKeyEncryptionKey.size()),
            policy.m_dataEncryptionKey.empty() ? NULL : policy.m_dataEncryptionKey.data(), //NULL if not present (when no wrapped key is present)
            static_cast<unsigned int>(policy.m_dataEncryptionKey.size()),
            ctx.m_bpsecReusableElementsInternal,
            NULL,
            true))
        {
            LOG_ERROR(subprocess) << "cannot encrypt bundle";
            return false;
        }
    }
    return true;
}


bool BpSecPolicyManager::FindPolicyAndProcessOutgoingBundle(BundleViewV7& bv, BpSecPolicyProcessingContext& ctx,
    const cbhe_eid_t& thisSecuritySourceEid) const
{
    const Bpv7CbhePrimaryBlock& primary = bv.m_primaryBlockView.header;
    const BpSecPolicy* bpSecPolicyPtr = FindPolicyWithCacheSupport(
        thisSecuritySourceEid, primary.m_sourceNodeId, primary.m_destinationEid, BPSEC_ROLE::SOURCE, ctx.m_searchCacheSource);
    if (bpSecPolicyPtr) {
        if (!BpSecPolicyManager::PopulateTargetArraysForSecuritySource(bv, ctx, *bpSecPolicyPtr)) {
            return false;
        }
        if (!BpSecPolicyManager::ProcessOutgoingBundle(bv, ctx, *bpSecPolicyPtr, thisSecuritySourceEid)) {
            return false;
        }
    }
    return true;
}

bool BpSecPolicyManager::LoadFromConfig(const BpSecConfig& config) {
    m_actionMaskSopMissingAtAcceptor = config.m_actionMaskSopMissingAtAcceptor;
    const policy_rules_vector_t& rulesVec = config.m_policyRulesVector;
    for (std::size_t ruleI = 0; ruleI < rulesVec.size(); ++ruleI) {
        const policy_rules_t& rule = rulesVec[ruleI];
        BPSEC_ROLE role;
        if (rule.m_securityRole == "source") {
            role = BPSEC_ROLE::SOURCE;
        }
        else if (rule.m_securityRole == "verifier") {
            role = BPSEC_ROLE::VERIFIER;
        }
        else if (rule.m_securityRole == "acceptor") {
            role = BPSEC_ROLE::ACCEPTOR;
        }
        else {
            LOG_ERROR(subprocess) << "Error loading BpSec config file: security role ("
                << rule.m_securityRole << ") is not any of the following: [source, verifier, acceptor].";
            return false;
        }
        bool isConfidentiality;
        if (rule.m_securityService == "confidentiality") {
            isConfidentiality = true;
        }
        else if (rule.m_securityService == "integrity") {
            isConfidentiality = false;
        }
        else {
            LOG_ERROR(subprocess) << "Error loading BpSec config file: securityService ("
                << rule.m_securityService << ") must be confidentiality or integrity";
            return false;
        }
        const bool isIntegrity = !isConfidentiality;

        BpSecPolicy policyToCopy; //initialized with defaults
        FragmentSet::data_fragment_set_t& blockTypeTargets = (isConfidentiality) ? policyToCopy.m_bcbBlockTypeTargets : policyToCopy.m_bibBlockTypeTargets;
        for (std::set<uint64_t>::const_iterator itBlockType = rule.m_securityTargetBlockTypes.cbegin();
            itBlockType != rule.m_securityTargetBlockTypes.cend(); ++itBlockType)
        {
            FragmentSet::InsertFragment(blockTypeTargets, FragmentSet::data_fragment_t(*itBlockType, *itBlockType));
        }

        for (std::size_t paramI = 0; paramI < rule.m_securityContextParamsVec.size(); ++paramI) {
            const security_context_param_t& param = rule.m_securityContextParamsVec[paramI];
            const BPSEC_SECURITY_CONTEXT_PARAM_NAME name = param.m_paramName;
            if (name == BPSEC_SECURITY_CONTEXT_PARAM_NAME::AES_VARIANT) {
                if (isIntegrity) {
                    LOG_ERROR(subprocess) << "Error loading BpSec config file: AES_VARIANT cannot be applied to integrity";
                    return false;
                }
                if (param.m_valueUint == 128) {
                    policyToCopy.m_confidentialityVariant = COSE_ALGORITHMS::A128GCM;
                }
                else if (param.m_valueUint == 256) {
                    policyToCopy.m_confidentialityVariant = COSE_ALGORITHMS::A256GCM;
                }
                else {
                    LOG_ERROR(subprocess) << "Error loading BpSec config file: aesVariant must be either 128 or 256";
                    return false;
                }
            }
            else if (name == BPSEC_SECURITY_CONTEXT_PARAM_NAME::SHA_VARIANT) {
                if (isConfidentiality) {
                    LOG_ERROR(subprocess) << "Error loading BpSec config file: SHA_VARIANT cannot be applied to confidentiality";
                    return false;
                }
                if (param.m_valueUint == 256) {
                    policyToCopy.m_integrityVariant = COSE_ALGORITHMS::HMAC_256_256;
                }
                else if (param.m_valueUint == 384) {
                    policyToCopy.m_integrityVariant = COSE_ALGORITHMS::HMAC_384_384;
                }
                else if (param.m_valueUint == 512) {
                    policyToCopy.m_integrityVariant = COSE_ALGORITHMS::HMAC_512_512;
                }
                else {
                    LOG_ERROR(subprocess) << "Error loading BpSec config file: shaVariant must be either 256 or 384 or 512";
                    return false;
                }
            }
            else if (name == BPSEC_SECURITY_CONTEXT_PARAM_NAME::IV_SIZE_BYTES) {
                if (isIntegrity) {
                    LOG_ERROR(subprocess) << "Error loading BpSec config file: IV_SIZE_BYTES cannot be applied to integrity";
                    return false;
                }
                if ((param.m_valueUint != 12) && (param.m_valueUint != 16))
                {
                    LOG_ERROR(subprocess) << "Error loading BpSec config file: IV_SIZE_BYTES must be either 12 or 16";
                    return false;
                }
                policyToCopy.m_use12ByteIv = (param.m_valueUint == 12);
            }
            else if (name == BPSEC_SECURITY_CONTEXT_PARAM_NAME::SCOPE_FLAGS) {
                if (isIntegrity) {
                    policyToCopy.m_integrityScopeMask = static_cast<BPSEC_BIB_HMAC_SHA2_INTEGRITY_SCOPE_MASKS>(param.m_valueUint);
                    if (param.m_valueUint > (static_cast<uint64_t>(BPSEC_BIB_HMAC_SHA2_INTEGRITY_SCOPE_MASKS::ALL_FLAGS_SET))) {
                        LOG_ERROR(subprocess) << "Error loading BpSec config file: BPSEC_BIB_HMAC_SHA2_INTEGRITY_SCOPE_MASKS is invalid";
                        return false;
                    }
                }
                else {
                    policyToCopy.m_aadScopeMask = static_cast<BPSEC_BCB_AES_GCM_AAD_SCOPE_MASKS>(param.m_valueUint);
                    if (param.m_valueUint > (static_cast<uint64_t>(BPSEC_BCB_AES_GCM_AAD_SCOPE_MASKS::ALL_FLAGS_SET))) {
                        LOG_ERROR(subprocess) << "Error loading BpSec config file: BPSEC_BCB_AES_GCM_AAD_SCOPE_MASKS is invalid";
                        return false;
                    }
                }
            }
            else if (name == BPSEC_SECURITY_CONTEXT_PARAM_NAME::SECURITY_BLOCK_CRC) {
                if (param.m_valueUint > (static_cast<uint64_t>(BPV7_CRC_TYPE::CRC32C))) {
                    LOG_ERROR(subprocess) << "Error loading BpSec config file: BPV7_CRC_TYPE is invalid";
                    return false;
                }
                if (isIntegrity) {
                    policyToCopy.m_bibCrcType = static_cast<BPV7_CRC_TYPE>(param.m_valueUint);
                }
                else {
                    policyToCopy.m_bcbCrcType = static_cast<BPV7_CRC_TYPE>(param.m_valueUint);
                }
            }
            else if ((name == BPSEC_SECURITY_CONTEXT_PARAM_NAME::KEY_ENCRYPTION_KEY_FILE) || (name == BPSEC_SECURITY_CONTEXT_PARAM_NAME::KEY_FILE)) {
                std::string fileContentsAsString;
                if (!JsonSerializable::LoadTextFileIntoString(param.m_valuePath, fileContentsAsString)) {
                    LOG_ERROR(subprocess) << "Error loading BpSec config file: cannot load key file: " << param.m_valuePath;
                    return false;
                }
                boost::trim(fileContentsAsString);
                std::vector<uint8_t> keyBytes;
                if ((!BinaryConversions::HexStringToBytes(fileContentsAsString, keyBytes)) || keyBytes.empty()) {
                    LOG_ERROR(subprocess) << "Error loading BpSec config file: invalid key inside file: " << param.m_valuePath;
                    return false;
                }
                if (name == BPSEC_SECURITY_CONTEXT_PARAM_NAME::KEY_ENCRYPTION_KEY_FILE) {
                    if (isIntegrity) {
                        policyToCopy.m_hmacKeyEncryptionKey = std::move(keyBytes);
                    }
                    else {
                        policyToCopy.m_confidentialityKeyEncryptionKey = std::move(keyBytes);
                    }

                }
                else { //KEY_FILE
                    if (isIntegrity) {
                        policyToCopy.m_hmacKey = std::move(keyBytes);
                    }
                    else {
                        policyToCopy.m_dataEncryptionKey = std::move(keyBytes);
                    }
                }
            }
            else {
                LOG_ERROR(subprocess) << "Error loading BpSec config file: invalid BPSEC_SECURITY_CONTEXT_PARAM_NAME " << ((int)name);
                return false;
            }
        }

        if (role == BPSEC_ROLE::SOURCE) {
            if (!policyToCopy.ValidateAndFinalize()) {
                LOG_ERROR(subprocess) << "Error loading BpSec config file: security source invalid";
                return false;
            }
        }
        if (isIntegrity) {
            if (policyToCopy.m_hmacKeyEncryptionKey.empty() && policyToCopy.m_hmacKey.empty()) {
                LOG_ERROR(subprocess) << "Error loading BpSec config file: no key specified for integrity";
                return false;
            }
            else if (policyToCopy.m_hmacKeyEncryptionKey.size() && policyToCopy.m_hmacKey.size()) {
                LOG_ERROR(subprocess) << "Error loading BpSec config file: both key and keyEncryptionKey specified for integrity.. ONLY SPECIFY ONE!";
                return false;
            }
        }
        else {
            if (policyToCopy.m_confidentialityKeyEncryptionKey.empty() && policyToCopy.m_dataEncryptionKey.empty()) {
                LOG_ERROR(subprocess) << "Error loading BpSec config file: no key specified for confidentiality";
                return false;
            }
            else if (policyToCopy.m_confidentialityKeyEncryptionKey.size() && policyToCopy.m_dataEncryptionKey.size()) {
                LOG_ERROR(subprocess) << "Error loading BpSec config file: both dataEncryptionKey and keyEncryptionKey specified for confidentiality.. ONLY SPECIFY ONE!";
                return false;
            }
        }

        for (std::set<std::string>::const_iterator itBundleSource = rule.m_bundleSource.cbegin();
            itBundleSource != rule.m_bundleSource.cend(); ++itBundleSource)
        {
            for (std::set<std::string>::const_iterator itBundleFinalDest = rule.m_bundleFinalDestination.cbegin();
                itBundleFinalDest != rule.m_bundleFinalDestination.cend(); ++itBundleFinalDest)
            {
                bool isNewPolicy;
                BpSecPolicy* policyPtr = CreateOrGetNewPolicy(
                    rule.m_securitySource,
                    *itBundleSource,
                    *itBundleFinalDest,
                    role, isNewPolicy);
                if (policyPtr == NULL) {
                    LOG_ERROR(subprocess) << "Error loading BpSec config file: cannot create new policy due to IPN syntax errors.";
                    return false;
                }
                if (!isNewPolicy) {
                    if (isConfidentiality && policyPtr->m_doConfidentiality) {
                        LOG_ERROR(subprocess) << "Error loading BpSec config file: a duplicate confidentiality policy rule was detected.";
                        return false;
                    }
                    else if (isIntegrity && policyPtr->m_doIntegrity) {
                        LOG_ERROR(subprocess) << "Error loading BpSec config file: a duplicate integrity policy rule was detected.";
                        return false;
                    }
                }
                
                if (isConfidentiality) {
                    policyPtr->m_doConfidentiality = true;
                    //confidentiality only variables
                    policyPtr->m_confidentialityVariant = policyToCopy.m_confidentialityVariant;
                    policyPtr->m_use12ByteIv = policyToCopy.m_use12ByteIv;
                    policyPtr->m_aadScopeMask = policyToCopy.m_aadScopeMask;
                    policyPtr->m_bcbCrcType = policyToCopy.m_bcbCrcType;
                    policyPtr->m_bcbBlockTypeTargets = policyToCopy.m_bcbBlockTypeTargets;
                    policyPtr->m_confidentialityKeyEncryptionKey = policyToCopy.m_confidentialityKeyEncryptionKey;
                    policyPtr->m_dataEncryptionKey = policyToCopy.m_dataEncryptionKey;
                    policyPtr->m_confidentialitySecurityFailureEventSetReferencePtr = rule.m_securityFailureEventSetReferencePtr;
                }
                else {
                    policyPtr->m_doIntegrity = true;
                    //integrity only variables
                    policyPtr->m_integrityVariant = policyToCopy.m_integrityVariant;
                    policyPtr->m_integrityScopeMask = policyToCopy.m_integrityScopeMask;
                    policyPtr->m_bibCrcType = policyToCopy.m_bibCrcType;
                    policyPtr->m_bibBlockTypeTargets = policyToCopy.m_bibBlockTypeTargets;
                    policyPtr->m_hmacKeyEncryptionKey = policyToCopy.m_hmacKeyEncryptionKey;
                    policyPtr->m_hmacKey = policyToCopy.m_hmacKey;
                    policyPtr->m_integritySecurityFailureEventSetReferencePtr = rule.m_securityFailureEventSetReferencePtr;
                }
                //fields set by ValidateAndFinalize()
                policyPtr->m_bcbTargetsPayloadBlock = policyToCopy.m_bcbTargetsPayloadBlock;
                policyPtr->m_bibMustBeEncrypted = policyToCopy.m_bibMustBeEncrypted;
            }
        }
    }
    return true;
}
