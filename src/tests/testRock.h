/*
 * Copyright (C) 1996-2023 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#ifndef SQUID_SRC_TEST_TESTROCK_H
#define SQUID_SRC_TEST_TESTROCK_H

#include "compat/cppunit.h"

/*
 * test the store framework
 */

class TestRock: public CPPUNIT_NS::TestFixture
{
    CPPUNIT_TEST_SUITE( TestRock );
    CPPUNIT_TEST( testRockCreate );
    CPPUNIT_TEST( testRockSwapOut );
    CPPUNIT_TEST_SUITE_END();

public:
    TestRock() : rr(nullptr) {}
    void setUp() override;
    void tearDown() override;

    typedef RefCount<Rock::SwapDir> SwapDirPointer;

protected:
    void commonInit();
    void storeInit();
    StoreEntry *createEntry(const int i);
    StoreEntry *addEntry(const int i);
    StoreEntry *getEntry(const int i);
    void testRockCreate();
    void testRockSwapOut();

private:
    SwapDirPointer store;
    Rock::SwapDirRr *rr;
};

#endif /* SQUID_SRC_TEST_TESTROCK_H */

