/*
 * Copyright 2017-2018 AVSystem <avsystem@avsystem.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <anjay_config.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <assert.h>

#include "dm_execute.h"

VISIBILITY_SOURCE_BEGIN

static anjay_execute_state_t state_read_value(anjay_execute_ctx_t* ctx, int ch);
static anjay_execute_state_t state_read_argument(anjay_execute_ctx_t* ctx, int ch);

static int next_char(anjay_execute_ctx_t *ctx) {
    if (ctx->end_of_message) {
        return EOF;
    }
    char buf[2];
    int result = anjay_get_string(ctx->input_ctx, buf, sizeof(buf));
    if (result < 0 || buf[0] == '\0') {
        ctx->end_of_message = true;
        return EOF;
    } else if (result == 0) {
        ctx->end_of_message = true;
    }
    return (int)(uint8_t)buf[0];
}

static bool is_arg_separator(int byte) {
    return byte == ',';
}

static bool is_value_delimiter(int byte) {
    return byte == '\'';
}

static bool is_value(int byte) {
    /* See OMA Specification Execute section, for more details. */
    return byte == '!'
        || (byte >= 0x23 && byte <= 0x26)
        || (byte >= 0x28 && byte <= 0x5b)
        || (byte >= 0x5d && byte <= 0x7e);
}

static bool is_value_assignment(int byte) {
    return byte == '=';
}

static int try_reading_next_arg(anjay_execute_ctx_t *ctx) {
    if (ctx->state == STATE_ERROR) {
        return -1;
    }
    ctx->arg = -1;
    ctx->arg_has_value = false;
    ctx->state = STATE_READ_ARGUMENT;

    while (ctx->state == STATE_READ_ARGUMENT) {
        ctx->state = state_read_argument(ctx, next_char(ctx));
    }

    if (ctx->arg == -1 && ctx->state == STATE_EOF) {
        return ANJAY_EXECUTE_GET_ARG_END;
    } else if (ctx->arg == -1 || ctx->state == STATE_ERROR) {
        return -1;
    }
    return 0;
}

static int skip_value(anjay_execute_ctx_t *ctx) {
    /*
     * If we are in the middle of reading the value assigned to the argument,
     * we'll skip the rest of it.
     */
    ssize_t ret = 0;
    if (ctx->state == STATE_READ_VALUE) {
        char buf[64];
        do {
            ret = anjay_execute_get_arg_value(ctx, buf, sizeof(buf));
        } while (ret > 0);
    }
    return (int) ret;
}

int anjay_execute_get_next_arg(anjay_execute_ctx_t *ctx,
                               int *out_arg,
                               bool *out_has_value) {
    if (skip_value(ctx)) {
        return -1;
    }

    if (ctx->state == STATE_EOF) {
        *out_arg = -1;
        *out_has_value = false;
        return ANJAY_EXECUTE_GET_ARG_END;
    }

    int result = try_reading_next_arg(ctx);
    if (result >= 0) {
        *out_arg = ctx->arg;
        *out_has_value = ctx->arg_has_value;
    }
    return result;
}

ssize_t anjay_execute_get_arg_value(anjay_execute_ctx_t *ctx,
                                    char *out_buf,
                                    size_t buf_size) {
    if (ctx->state != STATE_READ_VALUE) {
        return 0;
    } else if (buf_size < 2 || !out_buf) {
        return -1;
    }

    ssize_t read_bytes = 0;
    while ((size_t) read_bytes < buf_size-1) {
        int ch = next_char(ctx);
        ctx->state = state_read_value(ctx, ch);

        if (ctx->state == STATE_READ_VALUE) {
            out_buf[read_bytes++] = (char)ch;
        } else {
            break;
        }
    }
    out_buf[read_bytes] = '\0';

    if (ctx->state == STATE_ERROR) {
        return -1;
    }
    return read_bytes;
}

anjay_execute_ctx_t *_anjay_execute_ctx_create(anjay_input_ctx_t *ctx) {
    anjay_execute_ctx_t *ret =
            (anjay_execute_ctx_t *) avs_calloc(1, sizeof(anjay_execute_ctx_t));
    if (ret) {
        ret->input_ctx = ctx;
        ret->arg = -1;
        ret->state = STATE_READ_ARGUMENT;
    }
    return ret;
}

void _anjay_execute_ctx_destroy(anjay_execute_ctx_t **ctx) {
    if (ctx) {
        avs_free(*ctx);
        *ctx = NULL;
    }
}

static anjay_execute_state_t expect_separator_or_eof(anjay_execute_ctx_t *ctx,
                                                     int ch) {
    (void)ctx;
    if (is_arg_separator(ch)) {
        return STATE_FINISHED_READING_ARGUMENT;
    } else if (ch == EOF) {
        return STATE_EOF;
    }
    return STATE_ERROR;
}

static anjay_execute_state_t state_read_value(anjay_execute_ctx_t *ctx,
                                              int ch) {
    if (is_value(ch)) {
        return STATE_READ_VALUE;
    } else if (is_value_delimiter(ch)) {
        return expect_separator_or_eof(ctx, next_char(ctx));
    }
    return STATE_ERROR;
}

static anjay_execute_state_t expect_value(anjay_execute_ctx_t *ctx, int ch) {
    (void)ctx;
    if (is_value_delimiter(ch)) {
        return STATE_READ_VALUE;
    } else {
        return STATE_ERROR;
    }
}

static anjay_execute_state_t
expect_separator_or_assignment_or_eof(anjay_execute_ctx_t *ctx, int ch) {
    /*
     * This state is being entered only after some argument has been read
     * successfully, and it determines whether we should expect new argument, or
     * whether we should proceed with value read.
     */
    if (is_arg_separator(ch)) {
        ctx->arg_has_value = false;
        return STATE_FINISHED_READING_ARGUMENT;
    } else if (is_value_assignment(ch)) {
        ctx->arg_has_value = true;
        return expect_value(ctx, next_char(ctx));
    } else if (ch == EOF && ctx->end_of_message) {
        return STATE_EOF;
    }
    return STATE_ERROR;
}

static anjay_execute_state_t state_read_argument(anjay_execute_ctx_t *ctx,
                                                 int ch) {
    assert(ch == EOF || (0 <= ch && ch <= UINT8_MAX));

    if (isdigit(ch)) {
        ctx->arg = ch - '0';
        return expect_separator_or_assignment_or_eof(ctx, next_char(ctx));
    } else if (ch == EOF) {
        return STATE_EOF;
    }
    return STATE_ERROR;
}
