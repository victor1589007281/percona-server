/*
   Copyright (c) 2003, 2023, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

/**
 * @file ndb_types.h
 */

#ifndef NDB_TYPES_H
#define NDB_TYPES_H

#define NDB_SIZEOF_CHARP 8
#define NDB_SIZEOF_CHAR 1
#define NDB_SIZEOF_INT 4
#define NDB_SIZEOF_SHORT 2
#define NDB_SIZEOF_LONG 8
#define NDB_SIZEOF_LONG_LONG 8

#if defined(_WIN32)
typedef unsigned __int64 Uint64;
typedef   signed __int64 Int64;
#define NDB_EXPORT __declspec(dllexport)
#else
typedef unsigned long long Uint64;
typedef   signed long long Int64;
#define NDB_EXPORT
#endif

typedef   signed char  Int8;
typedef unsigned char  Uint8;
typedef   signed short Int16;
typedef unsigned short Uint16;
typedef   signed int   Int32;
typedef unsigned int   Uint32;

typedef Int64 ndb_off_t;

#ifndef INT_MIN64
#define INT_MIN64       (~0x7FFFFFFFFFFFFFFFLL)
#endif /* !INT_MIN64 */

#ifndef INT_MAX64
#define INT_MAX64       0x7FFFFFFFFFFFFFFFLL
#endif /* !INT_MAX64 */

#ifndef UINT_MAX64
#define UINT_MAX64      ((Uint64)(~0ULL))
#endif /* !UINT_MAX64 */

#define NDB_OFF_T_MAX INT_MAX64

typedef unsigned int UintR;

#ifdef __SIZE_TYPE__
  typedef __SIZE_TYPE__ UintPtr;
#elif NDB_SIZEOF_CHARP == 4
  typedef Uint32 UintPtr;
#elif NDB_SIZEOF_CHARP == 8
  typedef Uint64 UintPtr;
#else
  #error "Unknown size of (char *)"
#endif

#if ! (NDB_SIZEOF_CHAR == 1)
#error "Invalid define for Uint8"
#endif

#if ! (NDB_SIZEOF_SHORT == 2)
#error "Invalid define for Uint16"
#endif

#if ! (NDB_SIZEOF_INT == 4)
#error "Invalid define for Uint32"
#endif

#if ! (NDB_SIZEOF_LONG_LONG == 8)
#error "Invalid define for Uint64"
#endif

#include "ndb_constants.h"

#endif
