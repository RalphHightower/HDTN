#include <boost/test/unit_test.hpp>
#include "BundleStorageManagerMT.h"
#include "BundleStorageManagerAsio.h"
#include <iostream>
#include <string>
#include <boost/filesystem.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int_distribution.hpp>
#include <boost/timer/timer.hpp>
#include <boost/make_shared.hpp>
#include <boost/make_unique.hpp>
#include "SignalHandler.h"
#include "Environment.h"
#include "Sdnv.h"

static const uint64_t PRIMARY_SRC_NODE = 100;
static const uint64_t PRIMARY_SRC_SVC = 1;
//static const uint64_t PRIMARY_DEST_NODE = 300;
//static const uint64_t PRIMARY_DEST_SVC = 3;
//static const uint64_t PRIMARY_TIME = 1000;
//static const uint64_t PRIMARY_LIFETIME = 2000;
static const uint64_t PRIMARY_SEQ = 1;
static bool GenerateBundle(std::vector<uint8_t> & bundle, const bpv6_primary_block & primary, const uint64_t targetBundleSize, uint8_t startChar) {
    bundle.resize(targetBundleSize + 1000);
    uint8_t * buffer = &bundle[0];
    uint64_t payloadSize = targetBundleSize;
    uint8_t * const serializationBase = buffer;
    
    
    bpv6_canonical_block block;
    memset(&block, 0, sizeof(bpv6_canonical_block));

    
    uint64_t retVal;
    retVal = cbhe_bpv6_primary_block_encode(&primary, (char *)buffer, 0, 0);
    BOOST_REQUIRE_GT(retVal, 0);
    buffer += retVal;
    payloadSize -= retVal;

    block.type = BPV6_BLOCKTYPE_PAYLOAD;
    block.flags = BPV6_BLOCKFLAG_LAST_BLOCK;
    payloadSize -= 2;
    payloadSize -= SdnvGetNumBytesRequiredToEncode(payloadSize - 1); //as close as possible
    block.length = payloadSize;

    retVal = bpv6_canonical_block_encode(&block, (char *)buffer, 0, 0);
    BOOST_REQUIRE_GT(retVal, 0);
    buffer += retVal;
    for (uint64_t i = 0; i < payloadSize; ++i) {
        *buffer++ = startChar++;
    }

    const uint64_t bundleSize = buffer - serializationBase;
    BOOST_REQUIRE_GT(bundle.size(), bundleSize);
    bundle.resize(bundleSize);
    return (targetBundleSize == bundleSize);
}

//two days
#define NUMBER_OF_EXPIRATIONS (86400*2)

BOOST_AUTO_TEST_CASE(BundleStorageManagerAllTestCase)
{
    for (unsigned int whichBsm = 0; whichBsm < 2; ++whichBsm) {
        boost::random::mt19937 gen(static_cast<unsigned int>(std::time(0)));
        const boost::random::uniform_int_distribution<> distRandomData(0, 255);
        const boost::random::uniform_int_distribution<> distLinkId(0, 9);
        const boost::random::uniform_int_distribution<> distPriorityIndex(0, 2);
        const boost::random::uniform_int_distribution<> distAbsExpiration(0, NUMBER_OF_EXPIRATIONS - 1);


        static const cbhe_eid_t DEST_LINKS[10] = {
            cbhe_eid_t(1,1),
            cbhe_eid_t(2,1),
            cbhe_eid_t(3,1),
            cbhe_eid_t(4,1),
            cbhe_eid_t(5,1),
            cbhe_eid_t(6,1),
            cbhe_eid_t(7,1),
            cbhe_eid_t(8,1),
            cbhe_eid_t(9,1),
            cbhe_eid_t(10,1)
        };
        const std::vector<cbhe_eid_t> availableDestLinks = {
            cbhe_eid_t(1,1),
            cbhe_eid_t(2,1),
            cbhe_eid_t(3,1),
            cbhe_eid_t(4,1),
            cbhe_eid_t(5,1),
            cbhe_eid_t(6,1),
            cbhe_eid_t(7,1),
            cbhe_eid_t(8,1),
            cbhe_eid_t(9,1),
            cbhe_eid_t(10,1)
        };


        std::unique_ptr<BundleStorageManagerBase> bsmPtr;
        StorageConfig_ptr ptrStorageConfig = StorageConfig::CreateFromJsonFile((Environment::GetPathHdtnSourceRoot() / "tests" / "config_files" / "storage" / "storageConfigRelativePaths.json").string());
        ptrStorageConfig->m_tryToRestoreFromDisk = false; //manually set this json entry
        ptrStorageConfig->m_autoDeleteFilesOnExit = true; //manually set this json entry
        if (whichBsm == 0) {
            std::cout << "create BundleStorageManagerMT" << std::endl;
            bsmPtr = boost::make_unique<BundleStorageManagerMT>(ptrStorageConfig);
        }
        else {
            std::cout << "create BundleStorageManagerAsio" << std::endl;
            bsmPtr = boost::make_unique<BundleStorageManagerAsio>(ptrStorageConfig);
        }
        BundleStorageManagerBase & bsm = *bsmPtr;

        bsm.Start();


        static const boost::uint64_t sizes[17] = {
            1,
            2,
            BUNDLE_STORAGE_PER_SEGMENT_SIZE - 2 ,
            BUNDLE_STORAGE_PER_SEGMENT_SIZE - 1 ,
            BUNDLE_STORAGE_PER_SEGMENT_SIZE - 0,
            BUNDLE_STORAGE_PER_SEGMENT_SIZE + 1,
            BUNDLE_STORAGE_PER_SEGMENT_SIZE + 2,

            2 * BUNDLE_STORAGE_PER_SEGMENT_SIZE - 2 ,
            2 * BUNDLE_STORAGE_PER_SEGMENT_SIZE - 1 ,
            2 * BUNDLE_STORAGE_PER_SEGMENT_SIZE - 0,
            2 * BUNDLE_STORAGE_PER_SEGMENT_SIZE + 1,
            2 * BUNDLE_STORAGE_PER_SEGMENT_SIZE + 2,

            1000 * BUNDLE_STORAGE_PER_SEGMENT_SIZE - 2 ,
            1000 * BUNDLE_STORAGE_PER_SEGMENT_SIZE - 1 ,
            1000 * BUNDLE_STORAGE_PER_SEGMENT_SIZE - 0,
            1000 * BUNDLE_STORAGE_PER_SEGMENT_SIZE + 1,
            1000 * BUNDLE_STORAGE_PER_SEGMENT_SIZE + 2,
        };
        BundleStorageManagerSession_ReadFromDisk sessionRead; //has a heap allocation so reuse it
        BundleStorageManagerSession_ReadFromDisk sessionRead2;
        for (unsigned int sizeI = 0; sizeI < 17; ++sizeI) {
            const uint64_t custodyId = sizeI;
            const uint64_t size = sizes[sizeI];
            //std::cout << "testing size " << size << "\n";
            std::vector<boost::uint8_t> data(size);
            std::vector<boost::uint8_t> dataReadBack(size);
            for (std::size_t i = 0; i < size; ++i) {
                data[i] = distRandomData(gen);
            }
            const unsigned int linkId = distLinkId(gen);
            const unsigned int priorityIndex = distPriorityIndex(gen);
            const uint64_t absExpiration = distAbsExpiration(gen);

            BundleStorageManagerSession_WriteToDisk sessionWrite;
            bpv6_primary_block primary;
            memset(&primary, 0, sizeof(bpv6_primary_block));
            primary.version = 6;
            primary.flags = bpv6_bundle_set_priority(priorityIndex) |
                bpv6_bundle_set_gflags(BPV6_BUNDLEFLAG_SINGLETON | BPV6_BUNDLEFLAG_NOFRAGMENT);
            primary.src_node = PRIMARY_SRC_NODE;
            primary.src_svc = PRIMARY_SRC_SVC;
            primary.dst_node = DEST_LINKS[linkId].nodeId;
            primary.dst_svc = DEST_LINKS[linkId].serviceId;
            primary.custodian_node = 0;
            primary.custodian_svc = 0;
            primary.creation = 0;
            primary.lifetime = absExpiration;
            primary.sequence = PRIMARY_SEQ;

            //std::cout << "writing\n";
            uint64_t totalSegmentsRequired = bsm.Push(sessionWrite, primary, size);
            //std::cout << "totalSegmentsRequired " << totalSegmentsRequired << "\n";
            BOOST_REQUIRE_NE(totalSegmentsRequired, 0);

            const uint64_t totalBytesPushed = bsm.PushAllSegments(sessionWrite, primary, custodyId, data.data(), data.size());
            BOOST_REQUIRE_EQUAL(totalBytesPushed, data.size());


            //std::cout << "reading\n";
            
            uint64_t bytesToReadFromDisk = bsm.PopTop(sessionRead, availableDestLinks);
            //std::cout << "bytesToReadFromDisk " << bytesToReadFromDisk << "\n";
            BOOST_REQUIRE_EQUAL(bytesToReadFromDisk, size);

            //return top then take out again
            bsm.ReturnTop(sessionRead);
            bytesToReadFromDisk = bsm.PopTop(sessionRead, availableDestLinks);
            //std::cout << "bytesToReadFromDisk after returned " << bytesToReadFromDisk << "\n";
            BOOST_REQUIRE_EQUAL(bytesToReadFromDisk, size);

            //check if custody was taken (should be empty)
            {
                uint64_t bytesToReadFromDisk2 = bsm.PopTop(sessionRead2, availableDestLinks);
                //std::cout << "bytesToReadFromDisk2 " << bytesToReadFromDisk2 << "\n";
                BOOST_REQUIRE_EQUAL(bytesToReadFromDisk2, 0);
            }

            BOOST_REQUIRE_MESSAGE(dataReadBack != data, "dataReadBack should not equal data yet");

            BOOST_REQUIRE(bsm.ReadAllSegments(sessionRead, dataReadBack));

            //std::cout << "totalBytesRead " << totalBytesRead << "\n";
            BOOST_REQUIRE_MESSAGE(dataReadBack == data, "dataReadBack does not equal data");
            BOOST_REQUIRE_MESSAGE(bsm.RemoveReadBundleFromDisk(sessionRead), "error freeing bundle from disk");

        }
    }
}








BOOST_AUTO_TEST_CASE(BundleStorageManagerAll_RestoreFromDisk_TestCase)
{
    for (unsigned int whichBsm = 0; whichBsm < 2; ++whichBsm) {
        boost::random::mt19937 gen(static_cast<unsigned int>(std::time(0)));
        const boost::random::uniform_int_distribution<> distRandomData(0, 255);
        const boost::random::uniform_int_distribution<> distPriorityIndex(0, 2);

        static const cbhe_eid_t DEST_LINKS[10] = {
            cbhe_eid_t(1,1),
            cbhe_eid_t(2,1),
            cbhe_eid_t(3,1),
            cbhe_eid_t(4,1),
            cbhe_eid_t(5,1),
            cbhe_eid_t(6,1),
            cbhe_eid_t(7,1),
            cbhe_eid_t(8,1),
            cbhe_eid_t(9,1),
            cbhe_eid_t(10,1)
        };
        const std::vector<cbhe_eid_t> availableDestLinks = {
            cbhe_eid_t(1,1),
            cbhe_eid_t(2,1),
            cbhe_eid_t(3,1),
            cbhe_eid_t(4,1),
            cbhe_eid_t(5,1),
            cbhe_eid_t(6,1),
            cbhe_eid_t(7,1),
            cbhe_eid_t(8,1),
            cbhe_eid_t(9,1),
            cbhe_eid_t(10,1)
        };
        const std::vector<cbhe_eid_t> availableDestLinks2 = {cbhe_eid_t(2,1)};




        static const uint64_t sizes[15] = {
            BUNDLE_STORAGE_PER_SEGMENT_SIZE - 2,
            BUNDLE_STORAGE_PER_SEGMENT_SIZE - 1,
            BUNDLE_STORAGE_PER_SEGMENT_SIZE - 0,
            BUNDLE_STORAGE_PER_SEGMENT_SIZE + 1,
            BUNDLE_STORAGE_PER_SEGMENT_SIZE + 2,

            2 * BUNDLE_STORAGE_PER_SEGMENT_SIZE - 2,
            2 * BUNDLE_STORAGE_PER_SEGMENT_SIZE - 1,
            2 * BUNDLE_STORAGE_PER_SEGMENT_SIZE - 0,
            2 * BUNDLE_STORAGE_PER_SEGMENT_SIZE + 1,
            2 * BUNDLE_STORAGE_PER_SEGMENT_SIZE + 2,

            1000 * BUNDLE_STORAGE_PER_SEGMENT_SIZE - 2,
            1000 * BUNDLE_STORAGE_PER_SEGMENT_SIZE - 1,
            1000 * BUNDLE_STORAGE_PER_SEGMENT_SIZE - 0,
            1000 * BUNDLE_STORAGE_PER_SEGMENT_SIZE + 1,
            1000 * BUNDLE_STORAGE_PER_SEGMENT_SIZE + 2,
        };
        std::map < uint64_t, std::vector<uint8_t> > mapBundleSizeToBundleData;
        std::map < uint64_t, bpv6_primary_block > mapBundleSizeToPrimary;

        uint64_t bytesWritten = 0, totalSegmentsWritten = 0;
        backup_memmanager_t backup;

        {
            std::unique_ptr<BundleStorageManagerBase> bsmPtr;
            StorageConfig_ptr ptrStorageConfig = StorageConfig::CreateFromJsonFile((Environment::GetPathHdtnSourceRoot() / "tests" / "config_files" / "storage" / "storageConfigRelativePaths.json").string());
            ptrStorageConfig->m_tryToRestoreFromDisk = false; //manually set this json entry
            ptrStorageConfig->m_autoDeleteFilesOnExit = false; //manually set this json entry
            if (whichBsm == 0) {
                std::cout << "create BundleStorageManagerMT for Restore" << std::endl;
                bsmPtr = boost::make_unique<BundleStorageManagerMT>(ptrStorageConfig);
            }
            else {
                std::cout << "create BundleStorageManagerAsio for Restore" << std::endl;
                bsmPtr = boost::make_unique<BundleStorageManagerAsio>(ptrStorageConfig);
            }
            BundleStorageManagerBase & bsm = *bsmPtr;

            bsm.Start();

            uint64_t deletedMiddleBundleSize = 0;

            for (unsigned int sizeI = 0; sizeI < 15; ++sizeI) {
                const uint64_t custodyId = sizeI;
                const uint64_t targetBundleSize = sizes[sizeI];
                
                const unsigned int linkId = (sizeI == 12) ? 1 : 0;
                
                const unsigned int priorityIndex = distPriorityIndex(gen);
                const uint64_t absExpiration = sizeI;

                BundleStorageManagerSession_WriteToDisk sessionWrite;
                bpv6_primary_block primary;
                memset(&primary, 0, sizeof(bpv6_primary_block));
                primary.version = 6;
                primary.flags = bpv6_bundle_set_priority(priorityIndex) |
                    bpv6_bundle_set_gflags(BPV6_BUNDLEFLAG_SINGLETON | BPV6_BUNDLEFLAG_NOFRAGMENT);
                primary.src_node = PRIMARY_SRC_NODE;
                primary.src_svc = PRIMARY_SRC_SVC;
                primary.dst_node = DEST_LINKS[linkId].nodeId;
                primary.dst_svc = DEST_LINKS[linkId].serviceId;
                primary.custodian_node = 0;
                primary.custodian_svc = 0;
                primary.creation = 0;
                primary.lifetime = absExpiration;
                primary.sequence = PRIMARY_SEQ;
                std::vector<uint8_t> bundle;
                BOOST_REQUIRE(GenerateBundle(bundle, primary, targetBundleSize, static_cast<uint8_t>(sizeI)));
                //std::cout << "generate bundle of size " << bundle.size() << std::endl;
                //std::cout << "writing\n";
                uint64_t totalSegmentsRequired = bsm.Push(sessionWrite, primary, bundle.size());
                
                //std::cout << "totalSegmentsRequired " << totalSegmentsRequired << "\n";
                BOOST_REQUIRE_NE(totalSegmentsRequired, 0);

                const uint64_t totalBytesPushed = bsm.PushAllSegments(sessionWrite, primary, custodyId, bundle.data(), bundle.size());
                BOOST_REQUIRE_EQUAL(totalBytesPushed, bundle.size());
                
                if (sizeI != 12) {
                    bytesWritten += bundle.size();
                    totalSegmentsWritten += totalSegmentsRequired;
                    const uint64_t bundleSize = bundle.size();
                    mapBundleSizeToBundleData[bundleSize] = std::move(bundle);
                    mapBundleSizeToPrimary[bundleSize] = primary;
                }
                else {
                    deletedMiddleBundleSize = bundle.size();
                }
            }

            //delete a middle out
            BundleStorageManagerSession_ReadFromDisk sessionRead;
            boost::uint64_t bytesToReadFromDisk = bsm.PopTop(sessionRead, availableDestLinks2);
            BOOST_REQUIRE_EQUAL(bytesToReadFromDisk, deletedMiddleBundleSize);
            BOOST_REQUIRE_MESSAGE(bsm.RemoveReadBundleFromDisk(sessionRead), "error force freeing bundle from disk");

            bsm.GetMemoryManagerConstRef().BackupDataToVector(backup);
            BOOST_REQUIRE(bsm.GetMemoryManagerConstRef().IsBackupEqual(backup));
        }

        std::cout << "wrote bundles but leaving files\n";
        //boost::this_thread::sleep(boost::posix_time::milliseconds(500));
        std::cout << "restoring...\n";
        {
            std::unique_ptr<BundleStorageManagerBase> bsmPtr;
            StorageConfig_ptr ptrStorageConfig = StorageConfig::CreateFromJsonFile((Environment::GetPathHdtnSourceRoot() / "tests" / "config_files" / "storage" / "storageConfigRelativePaths.json").string());
            ptrStorageConfig->m_tryToRestoreFromDisk = true; //manually set this json entry
            ptrStorageConfig->m_autoDeleteFilesOnExit = true; //manually set this json entry
            if (whichBsm == 0) {
                std::cout << "create BundleStorageManagerMT for Restore" << std::endl;
                bsmPtr = boost::make_unique<BundleStorageManagerMT>(ptrStorageConfig);
            }
            else {
                std::cout << "create BundleStorageManagerAsio for Restore" << std::endl;
                bsmPtr = boost::make_unique<BundleStorageManagerAsio>(ptrStorageConfig);
            }
            BundleStorageManagerBase & bsm = *bsmPtr;


            
            //BOOST_REQUIRE(!bsm.GetMemoryManagerConstRef().IsBackupEqual(backup));
            BOOST_REQUIRE_MESSAGE(bsm.m_successfullyRestoredFromDisk, "error restoring from disk");
            BOOST_REQUIRE(bsm.GetMemoryManagerConstRef().IsBackupEqual(backup));
            std::cout << "restored\n";
            BOOST_REQUIRE_EQUAL(bsm.m_totalBundlesRestored, (15 - 1));
            BOOST_REQUIRE_EQUAL(bsm.m_totalBytesRestored, bytesWritten);
            BOOST_REQUIRE_EQUAL(bsm.m_totalSegmentsRestored, totalSegmentsWritten);

            bsm.Start();


            BOOST_REQUIRE_EQUAL(mapBundleSizeToBundleData.size(), 15-1);

            uint64_t totalBytesReadFromRestored = 0, totalSegmentsReadFromRestored = 0;
            BundleStorageManagerSession_ReadFromDisk sessionRead; //contains heap allocation so reuse it
            for (unsigned int sizeI = 0; sizeI < (15 - 1); ++sizeI) {


                //std::cout << "reading\n";
                const uint64_t bytesToReadFromDisk = bsm.PopTop(sessionRead, availableDestLinks);
                //std::cout << "bytesToReadFromDisk " << bytesToReadFromDisk << "\n";
                BOOST_REQUIRE_NE(bytesToReadFromDisk, 0);
                std::vector<boost::uint8_t> dataReadBack(bytesToReadFromDisk);
                totalBytesReadFromRestored += bytesToReadFromDisk;

                const std::size_t numSegmentsToRead = sessionRead.catalogEntryPtr->segmentIdChainVec.size();
                totalSegmentsReadFromRestored += numSegmentsToRead;

                BOOST_REQUIRE(bsm.ReadAllSegments(sessionRead, dataReadBack));
                const std::size_t totalBytesRead = dataReadBack.size();

                //std::cout << "totalBytesRead " << totalBytesRead << "\n";
                BOOST_REQUIRE_EQUAL(totalBytesRead, bytesToReadFromDisk);
                BOOST_REQUIRE_EQUAL(mapBundleSizeToBundleData.count(totalBytesRead), 1);
                BOOST_REQUIRE_EQUAL(mapBundleSizeToBundleData[totalBytesRead].size(), totalBytesRead);
                BOOST_REQUIRE(mapBundleSizeToBundleData[totalBytesRead] == dataReadBack);
                BOOST_REQUIRE_EQUAL(sessionRead.catalogEntryPtr->destEid.nodeId, mapBundleSizeToPrimary[totalBytesRead].dst_node);
                BOOST_REQUIRE_EQUAL(sessionRead.catalogEntryPtr->GetPriorityIndex(), bpv6_bundle_get_priority(mapBundleSizeToPrimary[totalBytesRead].flags));

                BOOST_REQUIRE_MESSAGE(bsm.RemoveReadBundleFromDisk(sessionRead), "error freeing bundle from disk");

            }

            BOOST_REQUIRE_EQUAL(totalBytesReadFromRestored, bytesWritten);
            BOOST_REQUIRE_EQUAL(totalSegmentsReadFromRestored, totalSegmentsWritten);



        }
    }
}