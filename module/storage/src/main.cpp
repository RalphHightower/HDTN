/**
 * @file main.cpp
 * @author Brian Tomko <brian.j.tomko@nasa.gov>
 *
 * @copyright Copyright (c) 2021 United States Government as represented by
 * the National Aeronautics and Space Administration.
 * No copyright is claimed in the United States under Title 17, U.S.Code.
 * All Other Rights Reserved.
 *
 * @section LICENSE
 * Released under the NASA Open Source Agreement (NOSA)
 * See LICENSE.md in the source root directory for more information.
 *
 * @section DESCRIPTION
 *
 * This file provides the "int main()" function to wrap StorageRunner
 * and forward command line arguments to StorageRunner.
 * This file is only used when running HDTN in distributed mode in which there
 * is a single process dedicated to the Storage module.
 */

#include "Logger.h"
#include "StartStorageRunner.h"

// LCOV_EXCL_START
int main(int argc, const char* argv[]) {
    int rVal;

    try {
        rVal = startStorageRunner(argc, argv);
    }
    catch (std::exception& e) {
        LOG_ERROR(hdtn::Logger::SubProcess::storage) << "error: " << e.what();
        return 1;
    }
    catch (...) {
        LOG_ERROR(hdtn::Logger::SubProcess::storage) << "Exception of unknown type!";
        return 1;
    }

    return rVal;
}
// LCOV_EXCL_STOP
