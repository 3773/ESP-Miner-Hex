#ifndef BM1366_H_
#define BM1366_H_

#include "asic_common.h"

typedef struct GlobalState GlobalState;
typedef struct bm_job bm_job;

#define BM1366_SERIALTX_DEBUG false
#define BM1366_SERIALRX_DEBUG false
#define BM1366_DEBUG_WORK false //causes insane amount of debug output
#define BM1366_DEBUG_JOBS false //causes insane amount of debug output

typedef struct __attribute__((__packed__))
{
    uint8_t job_id;
    uint8_t num_midstates;
    uint8_t starting_nonce[4];
    uint8_t nbits[4];
    uint8_t ntime[4];
    uint8_t merkle_root[32];
    uint8_t prev_block_hash[32];
    uint8_t version[4];
} BM1366_job;

uint8_t BM1366_init(GlobalState * GLOBAL_STATE);

// Bring-up hook for boards with stacked voltage domains (bitaxeHex).
// PREPARE is called before every detection attempt: the board code power
// cycles the core stack there, lets all chips run so the domains balance and
// gives a short reset pulse. COUNTED is called after every attempt with the
// number of chips that answered when asked one by one.
typedef enum {
    BM1366_BRINGUP_PREPARE,
    BM1366_BRINGUP_COUNTED,
} bm1366_bringup_stage_t;
typedef void (*bm1366_bringup_hook_t)(bm1366_bringup_stage_t stage, int attempt, int found);
void BM1366_set_bringup_hook(bm1366_bringup_hook_t hook);

void BM1366_send_work(GlobalState * GLOBAL_STATE, bm_job * next_bm_job);
void BM1366_set_version_mask(uint32_t version_mask);
int BM1366_set_max_baud(void);
float BM1366_send_hash_frequency(float frequency);
task_result * BM1366_process_work(GlobalState * GLOBAL_STATE);
void BM1366_read_registers(GlobalState * GLOBAL_STATE);
void BM1366_set_nonce_space(double nonce_percent, float frequency, uint16_t asic_count, uint16_t cores);

#endif /* BM1366_H_ */
