// TestMain.cpp
//------------------------------------------------------------------------------

// Includes
//------------------------------------------------------------------------------
#include "TestFramework/TestGroup.h"

// main
//------------------------------------------------------------------------------
int main( int, char *[] )
{
    // Tests to run
    REGISTER_TESTGROUP( TestArray )
    REGISTER_TESTGROUP( TestAtomic )
    REGISTER_TESTGROUP( TestAString )
    REGISTER_TESTGROUP( TestChainedMemoryStream )
    REGISTER_TESTGROUP( TestEnv )
    REGISTER_TESTGROUP( TestFileIO )
    REGISTER_TESTGROUP( TestFileStream )
    REGISTER_TESTGROUP( TestHash )
    REGISTER_TESTGROUP( TestLevenshteinDistance )
    REGISTER_TESTGROUP( TestMemInfo )
    REGISTER_TESTGROUP( TestMemPoolBlock )
    REGISTER_TESTGROUP( TestMutex )
    REGISTER_TESTGROUP( TestNetwork )
    REGISTER_TESTGROUP( TestPathUtils )
    REGISTER_TESTGROUP( TestReflection )
    REGISTER_TESTGROUP( TestSemaphore )
    REGISTER_TESTGROUP( TestSharedMemory )
    REGISTER_TESTGROUP( TestSmallBlockAllocator )
    REGISTER_TESTGROUP( TestSystemMutex )
    REGISTER_TESTGROUP( TestTestTCPConnectionPool )
    REGISTER_TESTGROUP( TestThread )
    REGISTER_TESTGROUP( TestThreadPool )
    REGISTER_TESTGROUP( TestTimer )
    REGISTER_TESTGROUP( TestUnorderedMap )

    TestManager utm;

    const bool allPassed = utm.RunTests();

    return allPassed ? 0 : -1;
}

//------------------------------------------------------------------------------
