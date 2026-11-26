/* SPDX-License-Identifier: MIT */
#ifndef TB_HARNESS_H
#define TB_HARNESS_H

#include <stdio.h>

extern int tb_test_fails;
extern int tb_test_checks;

#define CHECK(cond)                                                        \
	do {                                                               \
		tb_test_checks++;                                          \
		if (!(cond)) {                                             \
			tb_test_fails++;                                   \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,      \
				__LINE__, #cond);                          \
		}                                                          \
	} while (0)

#define CHECK_EQ_INT(a, b)                                                 \
	do {                                                               \
		long long _va = (long long)(a), _vb = (long long)(b);      \
		tb_test_checks++;                                          \
		if (_va != _vb) {                                          \
			tb_test_fails++;                                   \
			fprintf(stderr,                                    \
				"FAIL %s:%d: %s == %s (%lld != %lld)\n",   \
				__FILE__, __LINE__, #a, #b, _va, _vb);     \
		}                                                          \
	} while (0)

#define CHECK_STR(a, b)                                                    \
	do {                                                               \
		const char *_sa = (a), *_sb = (b);                         \
		tb_test_checks++;                                          \
		if (!_sa || !_sb || strcmp(_sa, _sb) != 0) {               \
			tb_test_fails++;                                   \
			fprintf(stderr,                                    \
				"FAIL %s:%d: %s == %s (\"%s\" != \"%s\")\n", \
				__FILE__, __LINE__, #a, #b, _sa ? _sa :  \
				"(null)", _sb ? _sb : "(null)");           \
		}                                                          \
	} while (0)

void test_elf32(void);
void test_syscalls(void);
void test_stack(void);
void test_engine(void);

#endif /* TB_HARNESS_H */
