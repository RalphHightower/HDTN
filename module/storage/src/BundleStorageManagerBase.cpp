/***************************************************************************
 * NASA Glenn Research Center, Cleveland, OH
 * Released under the NASA Open Source Agreement (NOSA)
 * May  2021
 *
 ****************************************************************************
 */

#include "BundleStorageManagerBase.h"
#include <iostream>
#include <string>
#include <boost/filesystem.hpp>
#include <boost/make_shared.hpp>
#include <boost/make_unique.hpp>


 //#ifdef _MSC_VER //Windows tests
 //static const char * FILE_PATHS[NUM_STORAGE_THREADS] = { "map0.bin", "map1.bin", "map2.bin", "map3.bin" };
 //#else //Lambda Linux tests
 //static const char * FILE_PATHS[NUM_STORAGE_THREADS] = { "/mnt/sda1/test/map0.bin", "/mnt/sdb1/test/map1.bin", "/mnt/sdc1/test/map2.bin", "/mnt/sdd1/test/map3.bin" };
 //#endif

BundleStorageManagerBase::BundleStorageManagerBase() : BundleStorageManagerBase("storageConfig.json") {}

BundleStorageManagerBase::BundleStorageManagerBase(const std::string & jsonConfigFileName) : BundleStorageManagerBase(StorageConfig::CreateFromJsonFile(jsonConfigFileName)) {
    if (!m_storageConfigPtr) {
        std::cerr << "cannot open storage json config file: " << jsonConfigFileName << std::endl;
        hdtn::Logger::getInstance()->logError("storage", "cannot open storage json config file: " + jsonConfigFileName);
        return;
    }
}

BundleStorageManagerBase::BundleStorageManagerBase(const StorageConfig_ptr & storageConfigPtr) :
    m_storageConfigPtr(storageConfigPtr),
    M_NUM_STORAGE_DISKS((m_storageConfigPtr) ? static_cast<unsigned int>(m_storageConfigPtr->m_storageDiskConfigVector.size()) : 1),
    M_TOTAL_STORAGE_CAPACITY_BYTES((m_storageConfigPtr) ? m_storageConfigPtr->m_totalStorageCapacityBytes : 1),
    M_MAX_SEGMENTS(M_TOTAL_STORAGE_CAPACITY_BYTES / SEGMENT_SIZE),
    m_memoryManager(M_MAX_SEGMENTS),
    m_lockMainThread(m_mutexMainThread),
    m_filePathsVec(M_NUM_STORAGE_DISKS),
    m_filePathsAsStringVec(M_NUM_STORAGE_DISKS),
    m_circularIndexBuffersVec(M_NUM_STORAGE_DISKS, CircularIndexBufferSingleProducerSingleConsumerConfigurable(CIRCULAR_INDEX_BUFFER_SIZE)),
    m_autoDeleteFilesOnExit((m_storageConfigPtr) ? m_storageConfigPtr->m_autoDeleteFilesOnExit : false),
    m_successfullyRestoredFromDisk(false),
    m_totalBundlesRestored(0),
    m_totalBytesRestored(0),
    m_totalSegmentsRestored(0)
{
    if (!m_storageConfigPtr) {
        return;
    }

    if (m_storageConfigPtr->m_tryToRestoreFromDisk) {
        m_successfullyRestoredFromDisk = RestoreFromDisk(&m_totalBundlesRestored, &m_totalBytesRestored, &m_totalSegmentsRestored);
    }


    for (unsigned int diskId = 0; diskId < M_NUM_STORAGE_DISKS; ++diskId) {
        m_filePathsVec[diskId] = boost::filesystem::path(m_storageConfigPtr->m_storageDiskConfigVector[diskId].storeFilePath);
        m_filePathsAsStringVec[diskId] = m_filePathsVec[diskId].string();
    }


    if (M_MAX_SEGMENTS > MAX_MEMORY_MANAGER_SEGMENTS) {
        std::cerr << "MAX SEGMENTS GREATER THAN WHAT MEMORY MANAGER CAN HANDLE\n";
        hdtn::Logger::getInstance()->logError("storage", "MAX SEGMENTS GREATER THAN WHAT MEMORY MANAGER CAN HANDLE");
        return;
    }

    m_circularBufferBlockDataPtr = (uint8_t*)malloc(CIRCULAR_INDEX_BUFFER_SIZE * M_NUM_STORAGE_DISKS * SEGMENT_SIZE * sizeof(uint8_t));
    m_circularBufferSegmentIdsPtr = (segment_id_t*)malloc(CIRCULAR_INDEX_BUFFER_SIZE * M_NUM_STORAGE_DISKS * sizeof(segment_id_t));


}

BundleStorageManagerBase::~BundleStorageManagerBase() {

    free(m_circularBufferBlockDataPtr);
    free(m_circularBufferSegmentIdsPtr);

    for (unsigned int diskId = 0; diskId < M_NUM_STORAGE_DISKS; ++diskId) {
        const boost::filesystem::path & p = m_filePathsVec[diskId];

        if (m_autoDeleteFilesOnExit && boost::filesystem::exists(p)) {
            boost::filesystem::remove(p);
            std::cout << "deleted " << p.string() << std::endl;
        }
    }
}


const MemoryManagerTreeArray & BundleStorageManagerBase::GetMemoryManagerConstRef() {
    return m_memoryManager;
}


boost::uint64_t BundleStorageManagerBase::Push(BundleStorageManagerSession_WriteToDisk & session, bpv6_primary_block & bundlePrimaryBlock, const boost::uint64_t bundleSizeBytes) {
    chain_info_t & chainInfo = session.chainInfo;
    segment_id_chain_vec_t & segmentIdChainVec = chainInfo.second;
    const boost::uint64_t totalSegmentsRequired = (bundleSizeBytes / BUNDLE_STORAGE_PER_SEGMENT_SIZE) + ((bundleSizeBytes % BUNDLE_STORAGE_PER_SEGMENT_SIZE) == 0 ? 0 : 1);

    chainInfo.first = bundleSizeBytes;
    segmentIdChainVec.resize(totalSegmentsRequired);
    session.nextLogicalSegment = 0;

    session.destLinkId = bundlePrimaryBlock.dst_node;
    //The bits in positions 8 and 7 constitute a two-bit priority field indicating the bundle's priority, with higher values
    //being of higher priority : 00 = bulk, 01 = normal, 10 = expedited, 11 is reserved for future use.
    session.priorityIndex = (bundlePrimaryBlock.flags >> 7) & 3;
    session.absExpiration = bundlePrimaryBlock.creation + bundlePrimaryBlock.lifetime;

    if (m_memoryManager.AllocateSegments_ThreadSafe(segmentIdChainVec)) {
        //std::cout << "firstseg " << segmentIdChainVec[0] << "\n";
        return totalSegmentsRequired;
    }

    return 0;
}

int BundleStorageManagerBase::PushSegment(BundleStorageManagerSession_WriteToDisk & session, void * buf, std::size_t size) {
    chain_info_t & chainInfo = session.chainInfo;
    segment_id_chain_vec_t & segmentIdChainVec = chainInfo.second;

    if (session.nextLogicalSegment >= segmentIdChainVec.size()) {
        return 0;
    }
    const boost::uint64_t bundleSizeBytes = (session.nextLogicalSegment == 0) ? chainInfo.first : UINT64_MAX;
    const segment_id_t segmentId = segmentIdChainVec[session.nextLogicalSegment++];
    const unsigned int diskIndex = segmentId % M_NUM_STORAGE_DISKS;
    CircularIndexBufferSingleProducerSingleConsumerConfigurable & cb = m_circularIndexBuffersVec[diskIndex];
    unsigned int produceIndex = cb.GetIndexForWrite();
    while (produceIndex == UINT32_MAX) { //store the volatile, wait until not full				
        m_conditionVariableMainThread.timed_wait(m_lockMainThread, boost::posix_time::milliseconds(10)); // call lock.unlock() and blocks the current thread
        //thread is now unblocked, and the lock is reacquired by invoking lock.lock()	
        produceIndex = cb.GetIndexForWrite();
    }

    boost::uint8_t * const circularBufferBlockDataPtr = &m_circularBufferBlockDataPtr[diskIndex * CIRCULAR_INDEX_BUFFER_SIZE * SEGMENT_SIZE];
    segment_id_t * const circularBufferSegmentIdsPtr = &m_circularBufferSegmentIdsPtr[diskIndex * CIRCULAR_INDEX_BUFFER_SIZE];


    boost::uint8_t * const dataCb = &circularBufferBlockDataPtr[produceIndex * SEGMENT_SIZE];
    circularBufferSegmentIdsPtr[produceIndex] = segmentId;
    m_circularBufferReadFromStoragePointers[diskIndex * CIRCULAR_INDEX_BUFFER_SIZE + produceIndex] = NULL; //isWriteToDisk = true

    const segment_id_t nextSegmentId = (session.nextLogicalSegment == segmentIdChainVec.size()) ? UINT32_MAX : segmentIdChainVec[session.nextLogicalSegment];
    memcpy(dataCb, &bundleSizeBytes, sizeof(bundleSizeBytes));
    memcpy(dataCb + sizeof(boost::uint64_t), &nextSegmentId, sizeof(nextSegmentId));
    memcpy(dataCb + SEGMENT_RESERVED_SPACE, buf, size);

    cb.CommitWrite();
    NotifyDiskOfWorkToDo_ThreadSafe(diskIndex);
    //std::cout << "writing " << size << " bytes\n";
    if (session.nextLogicalSegment == segmentIdChainVec.size()) {
        priority_array_t & priorityArray = m_destMap[session.destLinkId]; //created if not exist
        expiration_map_t & expirationMap = priorityArray[session.priorityIndex];
        chain_info_flist_t & chainInfoFlist = expirationMap[session.absExpiration];
        chainInfoFlist.push_front(std::move(chainInfo));
        //std::cout << "write complete\n";
    }

    return 1;
}


uint64_t BundleStorageManagerBase::PopTop(BundleStorageManagerSession_ReadFromDisk & session, const std::vector<uint64_t> & availableDestLinks) { //0 if empty, size if entry
    std::vector<priority_array_t *> priorityArrayPtrs;
    std::vector<uint64_t> priorityIndexToLinkIdVec;
    priorityArrayPtrs.reserve(availableDestLinks.size());
    priorityIndexToLinkIdVec.reserve(availableDestLinks.size());
    for (std::size_t i = 0; i < availableDestLinks.size(); ++i) {
        const uint64_t currentAvailableLink = availableDestLinks[i];
        destination_map_t::iterator dmIt = m_destMap.find(currentAvailableLink);
        if (dmIt != m_destMap.end()) {
            priority_array_t & priorityArrayRef = (dmIt->second);
            priorityArrayPtrs.push_back(&(dmIt->second));
            priorityIndexToLinkIdVec.push_back(currentAvailableLink);
        }
    }
    session.nextLogicalSegment = 0;
    session.nextLogicalSegmentToCache = 0;
    session.cacheReadIndex = 0;
    session.cacheWriteIndex = 0;

    //memset((uint8_t*)session.readCacheIsSegmentReady, 0, READ_CACHE_NUM_SEGMENTS_PER_SESSION);
    for (int i = NUMBER_OF_PRIORITIES - 1; i >= 0; --i) { //00 = bulk, 01 = normal, 10 = expedited
        abs_expiration_t lowestExpiration = UINT64_MAX;
        session.expirationMapPtr = NULL;
        session.chainInfoFlistPtr = NULL;

        for (std::size_t j = 0; j < priorityArrayPtrs.size(); ++j) {
            priority_array_t * priorityArray = priorityArrayPtrs[j];
            //std::cout << "size " << (*priorityVec).size() << "\n";
            expiration_map_t & expirationMap = (*priorityArray)[i];
            expiration_map_t::iterator it = expirationMap.begin();
            if (it != expirationMap.end()) {
                const abs_expiration_t thisExpiration = it->first;
                //std::cout << "thisexp " << thisExpiration << "\n";
                if (lowestExpiration > thisExpiration) {
                    lowestExpiration = thisExpiration;
                    //linkIndex = j;
                    session.expirationMapPtr = &expirationMap;
                    session.chainInfoFlistPtr = &it->second;
                    session.expirationMapIterator = it;
                    session.absExpiration = it->first;
                    session.destLinkId = priorityIndexToLinkIdVec[j];
                    session.priorityIndex = i;
                }
            }
        }
        if (session.chainInfoFlistPtr) {
            //have session take custody of data structure
            session.chainInfo = std::move(session.chainInfoFlistPtr->front());
            session.chainInfoFlistPtr->pop_front();

            if (session.chainInfoFlistPtr->empty()) {
                session.expirationMapPtr->erase(session.expirationMapIterator);
            }
            else {
                ////segmentIdVecPtr->shrink_to_fit();
            }
            return session.chainInfo.first; //use the front as new writes will be pushed back

        }

    }
    return 0;
}

bool BundleStorageManagerBase::ReturnTop(BundleStorageManagerSession_ReadFromDisk & session) { //0 if empty, size if entry
    (*session.expirationMapPtr)[session.absExpiration].push_front(std::move(session.chainInfo));
    return true;
}

std::size_t BundleStorageManagerBase::TopSegment(BundleStorageManagerSession_ReadFromDisk & session, void * buf) {
    segment_id_chain_vec_t & segments = session.chainInfo.second;

    while (((session.nextLogicalSegmentToCache - session.nextLogicalSegment) < READ_CACHE_NUM_SEGMENTS_PER_SESSION)
        && (session.nextLogicalSegmentToCache < segments.size()))
    {
        const segment_id_t segmentId = segments[session.nextLogicalSegmentToCache++];
        const unsigned int diskIndex = segmentId % M_NUM_STORAGE_DISKS;
        CircularIndexBufferSingleProducerSingleConsumerConfigurable & cb = m_circularIndexBuffersVec[diskIndex];
        unsigned int produceIndex = cb.GetIndexForWrite();
        while (produceIndex == UINT32_MAX) { //store the volatile, wait until not full				
            m_conditionVariableMainThread.timed_wait(m_lockMainThread, boost::posix_time::milliseconds(10)); // call lock.unlock() and blocks the current thread
            //thread is now unblocked, and the lock is reacquired by invoking lock.lock()	
            produceIndex = cb.GetIndexForWrite();
        }

        session.readCacheIsSegmentReady[session.cacheWriteIndex] = false;
        m_circularBufferIsReadCompletedPointers[diskIndex * CIRCULAR_INDEX_BUFFER_SIZE + produceIndex] = &session.readCacheIsSegmentReady[session.cacheWriteIndex];
        m_circularBufferReadFromStoragePointers[diskIndex * CIRCULAR_INDEX_BUFFER_SIZE + produceIndex] = &session.readCache[session.cacheWriteIndex * SEGMENT_SIZE];
        session.cacheWriteIndex = (session.cacheWriteIndex + 1) % READ_CACHE_NUM_SEGMENTS_PER_SESSION;
        m_circularBufferSegmentIdsPtr[diskIndex * CIRCULAR_INDEX_BUFFER_SIZE + produceIndex] = segmentId;

        cb.CommitWrite();
        NotifyDiskOfWorkToDo_ThreadSafe(diskIndex);
    }

    bool readIsReady = session.readCacheIsSegmentReady[session.cacheReadIndex];
    while (!readIsReady) { //store the volatile, wait until not full				
        m_conditionVariableMainThread.timed_wait(m_lockMainThread, boost::posix_time::milliseconds(10)); // call lock.unlock() and blocks the current thread
        //thread is now unblocked, and the lock is reacquired by invoking lock.lock()	
        readIsReady = session.readCacheIsSegmentReady[session.cacheReadIndex];
    }

    boost::uint64_t bundleSizeBytes;
    memcpy(&bundleSizeBytes, (void*)&session.readCache[session.cacheReadIndex * SEGMENT_SIZE + 0], sizeof(bundleSizeBytes));
    if (session.nextLogicalSegment == 0 && bundleSizeBytes != session.chainInfo.first) {// ? chainInfo.first : UINT64_MAX;
        std::cout << "error: read bundle size bytes = " << bundleSizeBytes << " does not match chainInfo = " << session.chainInfo.first << "\n";
        hdtn::Logger::getInstance()->logError("storage", "Error: read bundle size bytes = " + std::to_string(bundleSizeBytes) +
            " does not match chainInfo = " + std::to_string(session.chainInfo.first));
    }
    else if (session.nextLogicalSegment != 0 && bundleSizeBytes != UINT64_MAX) {// ? chainInfo.first : UINT64_MAX;
        std::cout << "error: read bundle size bytes = " << bundleSizeBytes << " is not UINT64_MAX\n";
        hdtn::Logger::getInstance()->logError("storage", "Error: read bundle size bytes = " + std::to_string(bundleSizeBytes) +
            " is not UINT64_MAX");
    }

    segment_id_t nextSegmentId;
    ++session.nextLogicalSegment;
    memcpy(&nextSegmentId, (void*)&session.readCache[session.cacheReadIndex * SEGMENT_SIZE + sizeof(bundleSizeBytes)], sizeof(nextSegmentId));
    if (session.nextLogicalSegment != session.chainInfo.second.size() && nextSegmentId != session.chainInfo.second[session.nextLogicalSegment]) {
        std::cout << "error: read nextSegmentId = " << nextSegmentId << " does not match chainInfo = " << session.chainInfo.second[session.nextLogicalSegment] << "\n";
        hdtn::Logger::getInstance()->logError("storage", "Error: read nextSegmentId = " + std::to_string(nextSegmentId) +
            " does not match chainInfo = " + std::to_string(session.chainInfo.second[session.nextLogicalSegment]));
    }
    else if (session.nextLogicalSegment == session.chainInfo.second.size() && nextSegmentId != UINT32_MAX) {
        std::cout << "error: read nextSegmentId = " << nextSegmentId << " is not UINT32_MAX\n";
        hdtn::Logger::getInstance()->logError("storage", "Error: read nextSegmentId = " + std::to_string(nextSegmentId) +
            " is not UINT32_MAX");
    }

    std::size_t size = BUNDLE_STORAGE_PER_SEGMENT_SIZE;
    if (nextSegmentId == UINT32_MAX) {
        boost::uint64_t modBytes = (session.chainInfo.first % BUNDLE_STORAGE_PER_SEGMENT_SIZE);
        if (modBytes != 0) {
            size = modBytes;
        }
    }

    memcpy(buf, (void*)&session.readCache[session.cacheReadIndex * SEGMENT_SIZE + SEGMENT_RESERVED_SPACE], size);
    session.cacheReadIndex = (session.cacheReadIndex + 1) % READ_CACHE_NUM_SEGMENTS_PER_SESSION;


    return size;
}
bool BundleStorageManagerBase::RemoveReadBundleFromDisk(BundleStorageManagerSession_ReadFromDisk & session, bool forceRemove) {
    if (!forceRemove && (session.nextLogicalSegment != session.chainInfo.second.size())) {
        std::cout << "error: bundle not yet read prior to removal\n";
        hdtn::Logger::getInstance()->logError("storage", "Error: bundle not yet read prior to removal");
        return false;
    }


    //destroy the head on the disk by writing UINT64_MAX to bundleSizeBytes of first logical segment
    chain_info_t & chainInfo = session.chainInfo;
    segment_id_chain_vec_t & segmentIdChainVec = chainInfo.second;


    const boost::uint64_t bundleSizeBytes = UINT64_MAX;
    const segment_id_t segmentId = segmentIdChainVec[0];
    const unsigned int diskIndex = segmentId % M_NUM_STORAGE_DISKS;
    CircularIndexBufferSingleProducerSingleConsumerConfigurable & cb = m_circularIndexBuffersVec[diskIndex];
    unsigned int produceIndex = cb.GetIndexForWrite();
    while (produceIndex == UINT32_MAX) { //store the volatile, wait until not full				
        m_conditionVariableMainThread.timed_wait(m_lockMainThread, boost::posix_time::milliseconds(10)); // call lock.unlock() and blocks the current thread
        //thread is now unblocked, and the lock is reacquired by invoking lock.lock()	
        produceIndex = cb.GetIndexForWrite();
    }

    boost::uint8_t * const circularBufferBlockDataPtr = &m_circularBufferBlockDataPtr[diskIndex * CIRCULAR_INDEX_BUFFER_SIZE * SEGMENT_SIZE];
    segment_id_t * const circularBufferSegmentIdsPtr = &m_circularBufferSegmentIdsPtr[diskIndex * CIRCULAR_INDEX_BUFFER_SIZE];


    boost::uint8_t * const dataCb = &circularBufferBlockDataPtr[produceIndex * SEGMENT_SIZE];
    circularBufferSegmentIdsPtr[produceIndex] = segmentId;
    m_circularBufferReadFromStoragePointers[diskIndex * CIRCULAR_INDEX_BUFFER_SIZE + produceIndex] = NULL; //isWriteToDisk = true

    memcpy(dataCb, &bundleSizeBytes, sizeof(bundleSizeBytes));


    cb.CommitWrite();
    NotifyDiskOfWorkToDo_ThreadSafe(diskIndex);

    return m_memoryManager.FreeSegments_ThreadSafe(segmentIdChainVec);
}
//uint64_t BundleStorageManagerMT::TopSegmentCount(BundleStorageManagerSession_ReadFromDisk & session) {
//	return session.chainInfoVecPtr->front().second.size(); //use the front as new writes will be pushed back
//}

bool BundleStorageManagerBase::RestoreFromDisk(uint64_t * totalBundlesRestored, uint64_t * totalBytesRestored, uint64_t * totalSegmentsRestored) {
    *totalBundlesRestored = 0; *totalBytesRestored = 0; *totalSegmentsRestored = 0;
    boost::uint8_t dataReadBuf[SEGMENT_SIZE];
    std::vector<FILE *> fileHandlesVec(M_NUM_STORAGE_DISKS);
    std::vector <boost::uint64_t> fileSizesVec(M_NUM_STORAGE_DISKS);
    for (unsigned int diskId = 0; diskId < M_NUM_STORAGE_DISKS; ++diskId) {
        const char * const filePath = m_storageConfigPtr->m_storageDiskConfigVector[diskId].storeFilePath.c_str();
        const boost::filesystem::path p(filePath);
        if (boost::filesystem::exists(p)) {
            fileSizesVec[diskId] = boost::filesystem::file_size(p);
            std::cout << "diskId " << diskId << " has file size of " << fileSizesVec[diskId] << "\n";
            hdtn::Logger::getInstance()->logInfo("storage", "Thread " + std::to_string(diskId) + " has file size of " +
                std::to_string(fileSizesVec[diskId]));
        }
        else {
            std::cout << "error: " << filePath << " does not exist\n";
            hdtn::Logger::getInstance()->logError("storage", "Error: " + std::string(filePath) + " does not exist");
            return false;
        }
        fileHandlesVec[diskId] = fopen(filePath, "rbR");
        if (fileHandlesVec[diskId] == NULL) {
            std::cout << "error opening file " << filePath << " for reading and restoring\n";
            hdtn::Logger::getInstance()->logError("storage", "Error opening file " + std::string(filePath) +
                " for reading and restoring");
            return false;
        }
    }

    bool restoreInProgress = true;
    for (segment_id_t potentialHeadSegmentId = 0; restoreInProgress; ++potentialHeadSegmentId) {
        if (!m_memoryManager.IsSegmentFree(potentialHeadSegmentId)) continue;
        segment_id_t segmentId = potentialHeadSegmentId;
        BundleStorageManagerSession_WriteToDisk session;
        chain_info_t & chainInfo = session.chainInfo;
        segment_id_chain_vec_t & segmentIdChainVec = chainInfo.second;
        bool headSegmentFound = false;
        for (session.nextLogicalSegment = 0; ; ++session.nextLogicalSegment) {
            const unsigned int diskIndex = segmentId % M_NUM_STORAGE_DISKS;
            FILE * const fileHandle = fileHandlesVec[diskIndex];
            const boost::uint64_t offsetBytes = static_cast<boost::uint64_t>(segmentId / M_NUM_STORAGE_DISKS) * SEGMENT_SIZE;
            const boost::uint64_t fileSize = fileSizesVec[diskIndex];
            if ((session.nextLogicalSegment == 0) && ((offsetBytes + SEGMENT_SIZE) > fileSize)) {
                std::cout << "end of restore\n";
                hdtn::Logger::getInstance()->logNotification("storage", "End of restore");
                restoreInProgress = false;
                break;
            }
#ifdef _MSC_VER 
            _fseeki64_nolock(fileHandle, offsetBytes, SEEK_SET);
#elif defined __APPLE__ 
            fseeko(fileHandle, offsetBytes, SEEK_SET);
#else
            fseeko64(fileHandle, offsetBytes, SEEK_SET);
#endif

            const std::size_t bytesReadFromFread = fread((void*)dataReadBuf, 1, SEGMENT_SIZE, fileHandle);
            if (bytesReadFromFread != SEGMENT_SIZE) {
                std::cout << "error reading at offset " << offsetBytes << " for disk " << diskIndex << " filesize " << fileSize << " logical segment " << session.nextLogicalSegment << " bytesread " << bytesReadFromFread << "\n";
                hdtn::Logger::getInstance()->logError("storage", "Error reading at offset " + std::to_string(offsetBytes) +
                    " for disk " + std::to_string(diskIndex) + " filesize " + std::to_string(fileSize) + " logical segment "
                    + std::to_string(session.nextLogicalSegment) + " bytesread " + std::to_string(bytesReadFromFread));
                return false;
        }

            boost::uint64_t bundleSizeBytes; // = (session.nextLogicalSegment == 0) ? chainInfo.first : UINT64_MAX;
            segment_id_t nextSegmentId; // = (session.nextLogicalSegment == segmentIdChainVec.size()) ? UINT32_MAX : segmentIdChainVec[session.nextLogicalSegment];

            memcpy(&bundleSizeBytes, dataReadBuf, sizeof(bundleSizeBytes));
            memcpy(&nextSegmentId, dataReadBuf + sizeof(boost::uint64_t), sizeof(nextSegmentId));

            if ((session.nextLogicalSegment == 0) && (bundleSizeBytes != UINT64_MAX)) { //head segment
                headSegmentFound = true;

                //copy bundle header and store to maps, push segmentId to chain vec
                bpv6_primary_block primary;
                std::size_t offset = cbhe_bpv6_primary_block_decode(&primary, (const char*)(dataReadBuf + SEGMENT_RESERVED_SPACE), 0, BUNDLE_STORAGE_PER_SEGMENT_SIZE);
                if (offset == 0) {
                    return false;//Malformed bundle
                }

                const boost::uint64_t totalSegmentsRequired = (bundleSizeBytes / BUNDLE_STORAGE_PER_SEGMENT_SIZE) + ((bundleSizeBytes % BUNDLE_STORAGE_PER_SEGMENT_SIZE) == 0 ? 0 : 1);

                //std::cout << "tot segs req " << totalSegmentsRequired << "\n";
                *totalBytesRestored += bundleSizeBytes;
                *totalSegmentsRestored += totalSegmentsRequired;
                chainInfo.first = bundleSizeBytes;
                segmentIdChainVec.resize(totalSegmentsRequired);

                session.destLinkId = primary.dst_node;
                //The bits in positions 8 and 7 constitute a two-bit priority field indicating the bundle's priority, with higher values
                //being of higher priority : 00 = bulk, 01 = normal, 10 = expedited, 11 is reserved for future use.
                session.priorityIndex = (primary.flags >> 7) & 3;
                session.absExpiration = primary.creation + primary.lifetime;


            }
            if (!headSegmentFound) break;
            if ((session.nextLogicalSegment) >= segmentIdChainVec.size()) {
                std::cout << "error: logical segment exceeds total segments required\n";
                hdtn::Logger::getInstance()->logError("storage", "Error: logical segment exceeds total segments required");
                return false;
            }
            if (!m_memoryManager.IsSegmentFree(segmentId)) {
                std::cout << "error: segmentId is already allocated\n";
                hdtn::Logger::getInstance()->logError("storage", "Error: segmentId is already allocated");
                return false;
            }
            m_memoryManager.AllocateSegmentId_NoCheck_NotThreadSafe(segmentId);
            segmentIdChainVec[session.nextLogicalSegment] = segmentId;



            if ((session.nextLogicalSegment + 1) >= segmentIdChainVec.size()) { //==
                if (nextSegmentId != UINT32_MAX) { //there are more segments
                    std::cout << "error: at the last logical segment but nextSegmentId != UINT32_MAX\n";
                    hdtn::Logger::getInstance()->logError("storage", "Error: at the last logical segment but nextSegmentId != UINT32_MAX");
                    return false;
                }
                priority_array_t & priorityArray = m_destMap[session.destLinkId]; //created if not exist
                expiration_map_t & expirationMap = priorityArray[session.priorityIndex];
                chain_info_flist_t & chainInfoFlist = expirationMap[session.absExpiration];
                chainInfoFlist.push_front(std::move(chainInfo));
                *totalBundlesRestored += 1;
                break;
                //std::cout << "write complete\n";
            }

            if (nextSegmentId == UINT32_MAX) { //there are more segments
                std::cout << "error: there are more logical segments but nextSegmentId == UINT32_MAX\n";
                hdtn::Logger::getInstance()->logError("storage", "Error: there are more logical segments but nextSegmentId == UINT32_MAX");
                return false;
            }
            segmentId = nextSegmentId;

    }
}


    for (unsigned int tId = 0; tId < M_NUM_STORAGE_DISKS; ++tId) {
        fclose(fileHandlesVec[tId]);
    }

    m_successfullyRestoredFromDisk = true;
    return true;
}


