#ifndef TNFS_TEST_H_
#define TNFS_TEST_H_

// Debug-only TNFS protocol test: MOUNT → OPENDIR("/") → READDIR loop → CLOSEDIR.
// Proves the full TNFS session flow before the real backend is implemented.

#ifdef SIDETNFS_DEBUG
void tnfs_test_run_once(void);
void tnfs_test_poll(void);
void tnfs_test_log_result(void);
#endif

#endif // TNFS_TEST_H_
