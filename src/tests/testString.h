/*
 * Copyright (C) 1996-2023 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

#ifndef SQUID_SRC_TEST_STRING_H
#define SQUID_SRC_TEST_STRING_H

#include "compat/cppunit.h"

/*
 * test the store framework
 */

class TestString: public CPPUNIT_NS::TestFixture
{
    CPPUNIT_TEST_SUITE( TestString );
    CPPUNIT_TEST( testCmpDefault );
    CPPUNIT_TEST( testCmpEmptyString );
    CPPUNIT_TEST( testCmpNotEmptyDefault );
    CPPUNIT_TEST( testSubstr );

    CPPUNIT_TEST_SUITE_END();

public:
    void setUp() override;

protected:
    void testCmpDefault();
    void testCmpEmptyString();
    void testCmpNotEmptyDefault();
    void testSubstr();
};

#endif

