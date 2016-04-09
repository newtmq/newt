#ifndef __UNIT_H__
#define __UNIT_H__

#include <CUnit/CUnit.h>
#include <kazusa/optparse.h>
#include <kazusa/config.h>
#include <kazusa/stomp.h>
#include <kazusa/list.h>

#include <errno.h>
#include <stdio.h>

#define CU_SUCCESS 0
#define CU_ERROR -1

int test_optparse(CU_pSuite);
int test_config(CU_pSuite);

#endif