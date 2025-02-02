//
// Copyright (c) 2022 ZettaScale Technology
//
// This program and the accompanying materials are made available under the
// terms of the Eclipse Public License 2.0 which is available at
// http://www.eclipse.org/legal/epl-2.0, or the Apache License, Version 2.0
// which is available at https://www.apache.org/licenses/LICENSE-2.0.
//
// SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
//
// Contributors:
//   ZettaScale Zenoh Team, <zenoh@zettascale.tech>
//

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "zenoh-pico.h"
#include "zenoh-pico/collections/bytes.h"
#include "zenoh-pico/protocol/core.h"

#undef NDEBUG
#include <assert.h>

#define MSG 10
#define MSG_LEN 1024
#define QRY 10
#define QRY_CLT 10
#define SET 10
#define SLEEP 1
#define TIMEOUT 60

const char *uri = "demo/example/";
unsigned int idx[SET];

// The active subscribers
_z_list_t *subs2 = NULL;

volatile unsigned int total = 0;

volatile unsigned int datas = 0;
void data_handler(const z_sample_t *sample, void *arg) {
    char *res = (char *)malloc(64);
    snprintf(res, 64, "%s%u", uri, *(unsigned int *)arg);
    printf(">> Received data: %s\t(%u/%u)\n", res, datas, total);

    z_owned_str_t k_str = z_keyexpr_to_string(sample->keyexpr);
    assert(sample->payload.len == MSG_LEN);
    assert(strlen(z_loan(k_str)) == strlen(res));
    assert(strncmp(res, z_loan(k_str), strlen(res)) == 0);
    (void)(sample);

    datas++;
    z_drop(z_move(k_str));
    free(res);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 1024);

    assert(argc == 2);
    (void)(argc);

    _Bool is_reliable = strncmp(argv[1], "tcp", 3) == 0;

    z_owned_config_t config = z_config_default();
    zp_config_insert(z_loan(config), Z_CONFIG_MODE_KEY, z_string_make("peer"));
    zp_config_insert(z_loan(config), Z_CONFIG_LISTEN_KEY, z_string_make(argv[1]));

    for (unsigned int i = 0; i < SET; i++) idx[i] = i;

    z_owned_session_t s1 = z_open(z_move(config));
    assert(z_check(s1));
    _z_bytes_t id_as_bytes =
        _z_bytes_wrap(z_loan(s1)._val.in->val._local_zid.id, _z_id_len(z_loan(s1)._val.in->val._local_zid));
    z_string_t zid1 = _z_string_from_bytes(&id_as_bytes);
    printf("Session 1 with PID: %s\n", zid1.val);
    _z_string_clear(&zid1);

    // Start the read session session lease loops
    zp_start_read_task(z_loan(s1), NULL);
    zp_start_lease_task(z_loan(s1), NULL);

    zp_sleep_s(SLEEP);

    config = z_config_default();
    zp_config_insert(z_loan(config), Z_CONFIG_MODE_KEY, z_string_make("peer"));
    zp_config_insert(z_loan(config), Z_CONFIG_CONNECT_KEY, z_string_make(argv[1]));

    z_owned_session_t s2 = z_open(z_move(config));
    assert(z_check(s2));
    id_as_bytes = _z_bytes_wrap(z_loan(s2)._val.in->val._local_zid.id, _z_id_len(z_loan(s2)._val.in->val._local_zid));
    z_string_t zid2 = _z_string_from_bytes(&id_as_bytes);
    printf("Session 2 with PID: %s\n", zid2.val);
    _z_string_clear(&zid2);

    // Start the read session session lease loops
    zp_start_read_task(z_loan(s2), NULL);
    zp_start_lease_task(z_loan(s2), NULL);

    zp_sleep_s(SLEEP * 5);

    // Declare subscribers on second session
    char *s1_res = (char *)malloc(64);
    for (unsigned int i = 0; i < SET; i++) {
        memset(s1_res, 0, 64);
        snprintf(s1_res, 64, "%s%u", uri, i);
        z_owned_closure_sample_t callback = z_closure(data_handler, NULL, &idx[i]);
        z_owned_subscriber_t *sub = (z_owned_subscriber_t *)zp_malloc(sizeof(z_owned_subscriber_t));
        *sub = z_declare_subscriber(z_loan(s2), z_keyexpr(s1_res), &callback, NULL);
        assert(z_check(*sub));
        printf("Declared subscription on session 2: %ju %zu %s\n", (uintmax_t)z_subscriber_loan(sub)._val->_entity_id,
               (z_zint_t)0, s1_res);
        subs2 = _z_list_push(subs2, sub);
    }

    // Write data from first session
    size_t len = MSG_LEN;
    uint8_t *payload = (uint8_t *)zp_malloc(len);
    memset(payload, 1, MSG_LEN);

    total = MSG * SET;
    for (unsigned int n = 0; n < MSG; n++) {
        for (unsigned int i = 0; i < SET; i++) {
            memset(s1_res, 0, 64);
            snprintf(s1_res, 64, "%s%u", uri, i);
            z_put_options_t opt = z_put_options_default();
            opt.congestion_control = Z_CONGESTION_CONTROL_BLOCK;
            z_put(z_loan(s1), z_keyexpr(s1_res), (const uint8_t *)payload, len, &opt);
            printf("Wrote data from session 1: %s %zu b\t(%u/%u)\n", s1_res, len, n * SET + (i + 1), total);
        }
    }

    // Wait to receive all the data
    zp_time_t now = zp_time_now();
    unsigned int expected = is_reliable ? total : 1;
    while (datas < expected) {
        assert(zp_time_elapsed_s(&now) < TIMEOUT);
        (void)(now);
        printf("Waiting for datas... %u/%u\n", datas, expected);
        zp_sleep_s(SLEEP);
    }
    if (is_reliable == true)
        assert(datas == expected);
    else
        assert(datas >= expected);
    datas = 0;

    zp_sleep_s(SLEEP);

    // Undeclare subscribers and queryables on second session
    while (subs2) {
        z_owned_subscriber_t *sub = _z_list_head(subs2);
        printf("Undeclared subscriber on session 2: %ju\n", (uintmax_t)z_subscriber_loan(sub)._val->_entity_id);
        z_undeclare_subscriber(z_move(*sub));
        subs2 = _z_list_pop(subs2, _z_noop_elem_free, NULL);
    }

    zp_sleep_s(SLEEP);

    // Stop both sessions
    printf("Stopping threads on session 1\n");
    zp_stop_read_task(z_loan(s1));
    zp_stop_lease_task(z_loan(s1));

    printf("Stopping threads on session 2\n");
    zp_stop_read_task(z_loan(s2));
    zp_stop_lease_task(z_loan(s2));

    // Close both sessions
    printf("Closing session 1\n");
    z_close(z_move(s1));

    zp_sleep_s(SLEEP);

    printf("Closing session 2\n");
    z_close(z_move(s2));

    zp_free((uint8_t *)payload);
    payload = NULL;

    free(s1_res);

    return 0;
}
