/* SPDX-License-Identifier: MIT */
#include "harness.h"

int tb_test_fails = 0;
int tb_test_checks = 0;

int main(void)
{
	test_elf32();
	test_syscalls();
	test_stack();
	test_engine();

	printf("checks: %d, failures: %d\n", tb_test_checks, tb_test_fails);
	printf("test_runner: %s\n", tb_test_fails ? "FAIL" : "PASS");
	return tb_test_fails ? 1 : 0;
}
