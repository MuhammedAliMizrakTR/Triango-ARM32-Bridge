/* SPDX-License-Identifier: MIT */
/*
 * Cross-built arm32 guest library for the engine's ELF integration test.
 * Build with arm-linux-gnueabi-gcc -shared -fPIC.
 */
int guest_add(int a, int b, int c, int d)
{
	return a + b + c + d;
}

int guest_sum(const int *arr, int n)
{
	int i, s = 0;

	for (i = 0; i < n; i++)
		s += arr[i];
	return s;
}

int guest_strlen(const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	return n;
}
