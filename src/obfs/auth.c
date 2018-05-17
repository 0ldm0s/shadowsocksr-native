#include <string.h>
#include <time.h>
#include <unistd.h>
#include "auth.h"
#include "obfsutil.h"
#include "crc32.h"
#include "base64.h"
#include "encrypt.h"
#include "obfs.h"
#include "ssrbuffer.h"

static int auth_simple_pack_unit_size = 2000;
typedef size_t (*hmac_with_key_func)(uint8_t *auth, const uint8_t *msg, size_t msg_len, const uint8_t *auth_key, size_t key_len);
typedef int (*hash_func)(char *auth, char *msg, int msg_len);

typedef struct _auth_simple_global_data {
    uint8_t local_client_id[8];
    uint32_t connection_id;
} auth_simple_global_data;

typedef struct _auth_simple_local_data {
    int has_sent_header;
    struct buffer_t * recv_buffer;
    uint32_t recv_id;
    uint32_t pack_id;
    char * salt;
    uint8_t * user_key;
    char uid[4];
    int user_key_len;
    hmac_with_key_func hmac;
    hash_func hash;
    int hash_len;
    int last_data_len;
    size_t unit_len;
} auth_simple_local_data;

void
auth_simple_local_data_init(auth_simple_local_data* local)
{
    local->has_sent_header = 0;
    local->recv_buffer = buffer_alloc(16384);
    local->recv_id = 1;
    local->pack_id = 1;
    local->salt = "";
    local->user_key = 0;
    local->user_key_len = 0;
    local->hmac = 0;
    local->hash = 0;
    local->hash_len = 0;
    local->salt = "";
    local->unit_len = 2000; // 8100
}

void *
auth_simple_init_data(void)
{
    auth_simple_global_data *global = (auth_simple_global_data*)malloc(sizeof(auth_simple_global_data));
    rand_bytes(global->local_client_id, 8);
    rand_bytes((uint8_t*)&global->connection_id, 4);
    global->connection_id &= 0xFFFFFF;
    return global;
}

struct obfs_t *
auth_simple_new_obfs(void)
{
    struct obfs_t * obfs = new_obfs();
    obfs->l_data = malloc(sizeof(auth_simple_local_data));
    auth_simple_local_data_init((auth_simple_local_data*)obfs->l_data);
    return obfs;
}

struct obfs_t *
auth_aes128_md5_new_obfs(void)
{
    struct obfs_t * obfs = new_obfs();
    obfs->l_data = malloc(sizeof(auth_simple_local_data));
    auth_simple_local_data_init((auth_simple_local_data*)obfs->l_data);
    ((auth_simple_local_data*)obfs->l_data)->hmac = ss_md5_hmac_with_key;
    ((auth_simple_local_data*)obfs->l_data)->hash = ss_md5_hash_func;
    ((auth_simple_local_data*)obfs->l_data)->hash_len = 16;
    ((auth_simple_local_data*)obfs->l_data)->salt = "auth_aes128_md5";
    return obfs;
}

struct obfs_t *
auth_aes128_sha1_new_obfs(void)
{
    struct obfs_t * obfs = new_obfs();
    obfs->l_data = malloc(sizeof(auth_simple_local_data));
    auth_simple_local_data_init((auth_simple_local_data*)obfs->l_data);
    ((auth_simple_local_data*)obfs->l_data)->hmac = ss_sha1_hmac_with_key;
    ((auth_simple_local_data*)obfs->l_data)->hash = ss_sha1_hash_func;
    ((auth_simple_local_data*)obfs->l_data)->hash_len = 20;
    ((auth_simple_local_data*)obfs->l_data)->salt = "auth_aes128_sha1";
    return obfs;
}

int
auth_aes128_sha1_get_overhead(struct obfs_t *obfs)
{
    return 9;
}

void
auth_simple_dispose(struct obfs_t *obfs)
{
    auth_simple_local_data *local = (auth_simple_local_data*)obfs->l_data;
    if (local->recv_buffer != NULL) {
        buffer_free(local->recv_buffer);
        local->recv_buffer = NULL;
    }
    if (local->user_key != NULL) {
        free(local->user_key);
        local->user_key = NULL;
    }
    free(local);
    obfs->l_data = NULL;
    dispose_obfs(obfs);
}

int
auth_simple_pack_data(char *data, int datalength, char *outdata)
{
    unsigned char rand_len = (xorshift128plus() & 0xF) + 1;
    int out_size = rand_len + datalength + 6;
    outdata[0] = (char)(out_size >> 8);
    outdata[1] = (char)(out_size);
    outdata[2] = (char)(rand_len);
    memmove(outdata + rand_len + 2, data, datalength);
    fillcrc32((unsigned char *)outdata, (unsigned int)out_size);
    return out_size;
}

int
auth_simple_pack_auth_data(auth_simple_global_data *global, char *data, int datalength, char *outdata)
{
    time_t t;
    unsigned char rand_len = (xorshift128plus() & 0xF) + 1;
    int out_size = rand_len + datalength + 6 + 12;
    outdata[0] = (char)(out_size >> 8);
    outdata[1] = (char)(out_size);
    outdata[2] = (char)(rand_len);
    ++global->connection_id;
    if (global->connection_id > 0xFF000000) {
        rand_bytes(global->local_client_id, 8);
        rand_bytes((uint8_t*)&global->connection_id, 4);
        global->connection_id &= 0xFFFFFF;
    }
    t = time(NULL);
    memintcopy_lt(outdata + rand_len + 2, (uint32_t)t);
    memmove(outdata + rand_len + 2 + 4, global->local_client_id, 4);
    memintcopy_lt(outdata + rand_len + 2 + 8, global->connection_id);
    memmove(outdata + rand_len + 2 + 12, data, datalength);
    fillcrc32((unsigned char *)outdata, (unsigned int)out_size);
    return out_size;
}

int
auth_simple_client_pre_encrypt(struct obfs_t *obfs, char **pplaindata, int datalength, size_t* capacity)
{
    char *plaindata = *pplaindata;
    auth_simple_local_data *local = (auth_simple_local_data*)obfs->l_data;
    char * out_buffer = (char*)malloc((size_t)(datalength * 2 + 64));
    char * buffer = out_buffer;
    char * data = plaindata;
    int len = datalength;
    int pack_len;
    if (len > 0 && local->has_sent_header == 0) {
        int head_size = get_head_size((const uint8_t *)plaindata, datalength, 30);
        if (head_size > datalength) {
            head_size = datalength;
        }
        pack_len = auth_simple_pack_auth_data((auth_simple_global_data *)obfs->server.g_data, data, head_size, buffer);
        buffer += pack_len;
        data += head_size;
        len -= head_size;
        local->has_sent_header = 1;
    }
    while ( len > auth_simple_pack_unit_size ) {
        pack_len = auth_simple_pack_data(data, auth_simple_pack_unit_size, buffer);
        buffer += pack_len;
        data += auth_simple_pack_unit_size;
        len -= auth_simple_pack_unit_size;
    }
    if (len > 0) {
        pack_len = auth_simple_pack_data(data, len, buffer);
        buffer += pack_len;
    }
    len = (int)(buffer - out_buffer);
    if ((int)(*capacity) < len) {
        *pplaindata = (char*)realloc(*pplaindata, *capacity = (size_t)(len * 2));
        plaindata = *pplaindata;
    }
    memmove(plaindata, out_buffer, len);
    free(out_buffer);
    return len;
}

ssize_t
auth_simple_client_post_decrypt(struct obfs_t *obfs, char **pplaindata, int datalength, size_t* capacity)
{
    int len;
    char * out_buffer;
    char * buffer;
    char *plaindata = *pplaindata;
    auth_simple_local_data *local = (auth_simple_local_data*)obfs->l_data;
    uint8_t * recv_buffer = (uint8_t *)local->recv_buffer->buffer;
    if (local->recv_buffer->len + datalength > 16384) {
        return -1;
    }
    buffer_concatenate(local->recv_buffer, plaindata, datalength);

    out_buffer = (char*)malloc((size_t)local->recv_buffer->len);
    buffer = out_buffer;
    while (local->recv_buffer->len > 2) {
        int crc;
        int data_size;
        int length = ((int)recv_buffer[0] << 8) | recv_buffer[1];
        if (length >= 8192 || length < 7) {
            free(out_buffer);
            local->recv_buffer->len = 0;
            return -1;
        }
        if (length > local->recv_buffer->len) {
            break;
        }
        crc = (int) crc32_imp((unsigned char*)recv_buffer, (unsigned int)length);
        if (crc != -1) {
            free(out_buffer);
            local->recv_buffer->len = 0;
            return -1;
        }
        data_size = length - recv_buffer[2] - 6;
        memmove(buffer, recv_buffer + 2 + recv_buffer[2], data_size);
        buffer += data_size;
        memmove(recv_buffer, recv_buffer + length, (local->recv_buffer->len -= length));
    }
    len = (int)(buffer - out_buffer);
    if ((int)*capacity < len) {
        *pplaindata = (char*)realloc(*pplaindata, *capacity = (size_t)(len * 2));
        plaindata = *pplaindata;
    }
    memmove(plaindata, out_buffer, len);
    free(out_buffer);
    return len;
}


int
auth_sha1_pack_data(char *data, int datalength, char *outdata)
{
    unsigned char rand_len = (xorshift128plus() & 0xF) + 1;
    int out_size = rand_len + datalength + 6;
    outdata[0] = (char)(out_size >> 8);
    outdata[1] = (char)out_size;
    outdata[2] = (char)rand_len;
    memmove(outdata + rand_len + 2, data, datalength);
    filladler32((unsigned char *)outdata, (unsigned int)out_size);
    return out_size;
}

int
auth_sha1_pack_auth_data(auth_simple_global_data *global, struct server_info_t *server, char *data, int datalength, char *outdata)
{
    time_t t;
    char hash[SHA1_BYTES];
    unsigned char rand_len = (xorshift128plus() & 0x7F) + 1;
    int data_offset = rand_len + 4 + 2;
    int out_size = data_offset + datalength + 12 + OBFS_HMAC_SHA1_LEN;
    fillcrc32to((unsigned char *)server->key, (unsigned int)server->key_len, (unsigned char *)outdata);
    outdata[4] = (char)(out_size >> 8);
    outdata[5] = (char)out_size;
    outdata[6] = (char)rand_len;
    ++global->connection_id;
    if (global->connection_id > 0xFF000000) {
        rand_bytes(global->local_client_id, 8);
        rand_bytes((uint8_t*)&global->connection_id, 4);
        global->connection_id &= 0xFFFFFF;
    }
    t = time(NULL);
    memintcopy_lt(outdata + data_offset, (uint32_t)t);
    memmove(outdata + data_offset + 4, global->local_client_id, 4);
    memintcopy_lt(outdata + data_offset + 8, global->connection_id);
    memmove(outdata + data_offset + 12, data, datalength);
    ss_sha1_hmac(hash, outdata, out_size - OBFS_HMAC_SHA1_LEN, server->iv, (int)server->iv_len, server->key, (int)server->key_len);
    memcpy(outdata + out_size - OBFS_HMAC_SHA1_LEN, hash, OBFS_HMAC_SHA1_LEN);
    return out_size;
}

int
auth_sha1_client_pre_encrypt(struct obfs_t *obfs, char **pplaindata, int datalength, size_t* capacity)
{
    char *plaindata = *pplaindata;
    auth_simple_local_data *local = (auth_simple_local_data*)obfs->l_data;
    char * out_buffer = (char*)malloc((size_t)(datalength * 2 + 256));
    char * buffer = out_buffer;
    char * data = plaindata;
    int len = datalength;
    int pack_len;
    if (len > 0 && local->has_sent_header == 0) {
        int head_size = get_head_size((const uint8_t *)plaindata, datalength, 30);
        if (head_size > datalength) {
            head_size = datalength;
        }
        pack_len = auth_sha1_pack_auth_data((auth_simple_global_data *)obfs->server.g_data, &obfs->server, data, head_size, buffer);
        buffer += pack_len;
        data += head_size;
        len -= head_size;
        local->has_sent_header = 1;
    }
    while ( len > auth_simple_pack_unit_size ) {
        pack_len = auth_sha1_pack_data(data, auth_simple_pack_unit_size, buffer);
        buffer += pack_len;
        data += auth_simple_pack_unit_size;
        len -= auth_simple_pack_unit_size;
    }
    if (len > 0) {
        pack_len = auth_sha1_pack_data(data, len, buffer);
        buffer += pack_len;
    }
    len = (int)(buffer - out_buffer);
    if ((int)*capacity < len) {
        *pplaindata = (char*)realloc(*pplaindata, *capacity = (size_t)(len * 2));
        plaindata = *pplaindata;
    }
    memmove(plaindata, out_buffer, len);
    free(out_buffer);
    return len;
}

ssize_t
auth_sha1_client_post_decrypt(struct obfs_t *obfs, char **pplaindata, int datalength, size_t* capacity)
{
    int len;
    char * buffer;
    char * out_buffer;
    char *plaindata = *pplaindata;
    auth_simple_local_data *local = (auth_simple_local_data*)obfs->l_data;
    uint8_t * recv_buffer = (uint8_t *)local->recv_buffer->buffer;
    if (local->recv_buffer->len + datalength > 16384) {
        return -1;
    }
    memmove(recv_buffer + local->recv_buffer->len, plaindata, datalength);
    local->recv_buffer->len += datalength;

    out_buffer = (char*)malloc((size_t)local->recv_buffer->len);
    buffer = out_buffer;
    while (local->recv_buffer->len > 2) {
        int pos;
        int data_size;
        int length = ((int)recv_buffer[0] << 8) | recv_buffer[1];
        if (length >= 8192 || length < 7) {
            free(out_buffer);
            local->recv_buffer->len = 0;
            return -1;
        }
        if (length > local->recv_buffer->len) {
            break;
        }
        if (checkadler32((unsigned char*)recv_buffer, (unsigned int)length) == 0) {
            free(out_buffer);
            local->recv_buffer->len = 0;
            return -1;
        }
        pos = recv_buffer[2] + 2;
        data_size = length - pos - 4;
        memmove(buffer, recv_buffer + pos, data_size);
        buffer += data_size;
        memmove(recv_buffer, recv_buffer + length, local->recv_buffer->len -= length);
    }
    len = (int)(buffer - out_buffer);
    if ((int)*capacity < len) {
        *pplaindata = (char*)realloc(*pplaindata, *capacity = (size_t)(len * 2));
        plaindata = *pplaindata;
    }
    memmove(plaindata, out_buffer, len);
    free(out_buffer);
    return (ssize_t)len;
}

int
auth_sha1_v2_pack_data(char *data, int datalength, char *outdata)
{
    unsigned int rand_len = (datalength > 1300 ? 0 : datalength > 400 ? (xorshift128plus() & 0x7F) : (xorshift128plus() & 0x3FF)) + 1;
    int out_size = (int)rand_len + datalength + 6;
    outdata[0] = (char)(out_size >> 8);
    outdata[1] = (char)out_size;
    if (rand_len < 128) {
        outdata[2] = (char)rand_len;
    } else {
        outdata[2] = (char)0xFF;
        outdata[3] = (char)(rand_len >> 8);
        outdata[4] = (char)rand_len;
    }
    memmove(outdata + rand_len + 2, data, datalength);
    filladler32((unsigned char *)outdata, (unsigned int)out_size);
    return out_size;
}

int
auth_sha1_v2_pack_auth_data(auth_simple_global_data *global, struct server_info_t *server, char *data, int datalength, char *outdata)
{
    char hash[SHA1_BYTES];
    unsigned int rand_len = (datalength > 1300 ? 0 : datalength > 400 ? (xorshift128plus() & 0x7F) : (xorshift128plus() & 0x3FF)) + 1;
    int data_offset = (int)rand_len + 4 + 2;
    int out_size = data_offset + datalength + 12 + OBFS_HMAC_SHA1_LEN;
    const char* salt = "auth_sha1_v2";
    int salt_len = (int) strlen(salt);
    unsigned char *crc_salt = (unsigned char*)malloc((size_t)salt_len + server->key_len);
    memcpy(crc_salt, salt, salt_len);
    memcpy(crc_salt + salt_len, server->key, server->key_len);
    fillcrc32to(crc_salt, (unsigned int)((size_t)salt_len + server->key_len), (unsigned char *)outdata);
    free(crc_salt);
    outdata[4] = (char)(out_size >> 8);
    outdata[5] = (char)out_size;
    if (rand_len < 128) {
        outdata[6] = (char)rand_len;
    } else {
        outdata[6] = (char)0xFF;
        outdata[7] = (char)(rand_len >> 8);
        outdata[8] = (char)rand_len;
    }
    ++global->connection_id;
    if (global->connection_id > 0xFF000000) {
        rand_bytes(global->local_client_id, 8);
        rand_bytes((uint8_t*)&global->connection_id, 4);
        global->connection_id &= 0xFFFFFF;
    }
    memmove(outdata + data_offset, global->local_client_id, 8);
    memintcopy_lt(outdata + data_offset + 8, global->connection_id);
    memmove(outdata + data_offset + 12, data, datalength);
    ss_sha1_hmac(hash, outdata, out_size - OBFS_HMAC_SHA1_LEN, server->iv, (int)server->iv_len, server->key, (int)server->key_len);
    memcpy(outdata + out_size - OBFS_HMAC_SHA1_LEN, hash, OBFS_HMAC_SHA1_LEN);
    return out_size;
}

int
auth_sha1_v2_client_pre_encrypt(struct obfs_t *obfs, char **pplaindata, int datalength, size_t* capacity)
{
    char *plaindata = *pplaindata;
    auth_simple_local_data *local = (auth_simple_local_data*)obfs->l_data;
    char * out_buffer = (char*)malloc((size_t)(datalength * 2 + (SSR_BUFF_SIZE * 2)));
    char * buffer = out_buffer;
    char * data = plaindata;
    int len = datalength;
    int pack_len;
    if (len > 0 && local->has_sent_header == 0) {
        int head_size = get_head_size((const uint8_t *)plaindata, datalength, 30);
        if (head_size > datalength) {
            head_size = datalength;
        }
        pack_len = auth_sha1_v2_pack_auth_data((auth_simple_global_data *)obfs->server.g_data, &obfs->server, data, head_size, buffer);
        buffer += pack_len;
        data += head_size;
        len -= head_size;
        local->has_sent_header = 1;
    }
    while ( len > auth_simple_pack_unit_size ) {
        pack_len = auth_sha1_v2_pack_data(data, auth_simple_pack_unit_size, buffer);
        buffer += pack_len;
        data += auth_simple_pack_unit_size;
        len -= auth_simple_pack_unit_size;
    }
    if (len > 0) {
        pack_len = auth_sha1_v2_pack_data(data, len, buffer);
        buffer += pack_len;
    }
    len = (int)(buffer - out_buffer);
    if ((int)*capacity < len) {
        *pplaindata = (char*)realloc(*pplaindata, *capacity = (size_t)(len * 2));
        plaindata = *pplaindata;
    }
    memmove(plaindata, out_buffer, len);
    free(out_buffer);
    return len;
}

ssize_t
auth_sha1_v2_client_post_decrypt(struct obfs_t *obfs, char **pplaindata, int datalength, size_t* capacity)
{
    int len;
    char error;
    char * buffer;
    char * out_buffer;
    char *plaindata = *pplaindata;
    auth_simple_local_data *local = (auth_simple_local_data*)obfs->l_data;
    uint8_t * recv_buffer = (uint8_t *)local->recv_buffer->buffer;
    if (local->recv_buffer->len + datalength > 16384) {
        return -1;
    }
    memmove(recv_buffer + local->recv_buffer->len, plaindata, datalength);
    local->recv_buffer->len += datalength;

    out_buffer = (char*)malloc((size_t)local->recv_buffer->len);
    buffer = out_buffer;
    error = 0;
    while (local->recv_buffer->len > 2) {
        int data_size;
        int pos;
        int length = ((int)recv_buffer[0] << 8) | recv_buffer[1];
        if (length >= 8192 || length < 7) {
            local->recv_buffer->len = 0;
            error = 1;
            break;
        }
        if (length > local->recv_buffer->len) {
            break;
        }
        if (checkadler32((unsigned char*)recv_buffer, (unsigned int)length) == 0) {
            local->recv_buffer->len = 0;
            error = 1;
            break;
        }
        pos = recv_buffer[2];
        if (pos < 255) {
            pos += 2;
        } else {
            pos = ((recv_buffer[3] << 8) | recv_buffer[4]) + 2;
        }
        data_size = length - pos - 4;
        memmove(buffer, recv_buffer + pos, data_size);
        buffer += data_size;
        memmove(recv_buffer, recv_buffer + length, local->recv_buffer->len -= length);
    }
    if (error == 0) {
        len = (int)(buffer - out_buffer);
        if ((int)*capacity < len) {
            *pplaindata = (char*)realloc(*pplaindata, *capacity = (size_t)(len * 2));
            plaindata = *pplaindata;
        }
        memmove(plaindata, out_buffer, len);
    } else {
        len = -1;
    }
    free(out_buffer);
    return (ssize_t) len;
}

int
auth_sha1_v4_pack_data(char *data, int datalength, char *outdata)
{
    uint32_t crc_val;
    unsigned int rand_len = (datalength > 1300 ? 0 : datalength > 400 ? (xorshift128plus() & 0x7F) : (xorshift128plus() & 0x3FF)) + 1;
    int out_size = (int)rand_len + datalength + 8;
    outdata[0] = (char)(out_size >> 8);
    outdata[1] = (char)out_size;
    crc_val = crc32_imp((unsigned char*)outdata, 2);
    outdata[2] = (char)crc_val;
    outdata[3] = (char)(crc_val >> 8);
    if (rand_len < 128) {
        outdata[4] = (char)rand_len;
    } else {
        outdata[4] = (char)0xFF;
        outdata[5] = (char)(rand_len >> 8);
        outdata[6] = (char)rand_len;
    }
    memmove(outdata + rand_len + 4, data, datalength);
    filladler32((unsigned char *)outdata, (unsigned int)out_size);
    return out_size;
}

int
auth_sha1_v4_pack_auth_data(auth_simple_global_data *global, struct server_info_t *server, char *data, int datalength, char *outdata)
{
    char hash[SHA1_BYTES];
    time_t t;
    unsigned int rand_len = (datalength > 1300 ? 0 : datalength > 400 ? (xorshift128plus() & 0x7F) : (xorshift128plus() & 0x3FF)) + 1;
    int data_offset = (int)rand_len + 4 + 2;
    int out_size = data_offset + datalength + 12 + OBFS_HMAC_SHA1_LEN;
    const char* salt = "auth_sha1_v4";
    int salt_len = (int)strlen(salt);
    unsigned char *crc_salt = (unsigned char*)malloc((size_t)salt_len + server->key_len + 2);
    crc_salt[0] = (unsigned char)(outdata[0] = (char)(out_size >> 8));
    crc_salt[1] = (unsigned char)(outdata[1] = (char)out_size);

    memcpy(crc_salt + 2, salt, salt_len);
    memcpy(crc_salt + salt_len + 2, server->key, server->key_len);
    fillcrc32to(crc_salt, (unsigned int)((size_t)salt_len + server->key_len + 2), (unsigned char *)outdata + 2);
    free(crc_salt);
    if (rand_len < 128) {
        outdata[6] = (char)rand_len;
    } else {
        outdata[6] = (char)0xFF;
        outdata[7] = (char)(rand_len >> 8);
        outdata[8] = (char)rand_len;
    }
    ++global->connection_id;
    if (global->connection_id > 0xFF000000) {
        rand_bytes(global->local_client_id, 8);
        rand_bytes((uint8_t*)&global->connection_id, 4);
        global->connection_id &= 0xFFFFFF;
    }
    t = time(NULL);
    memintcopy_lt(outdata + data_offset, (uint32_t)t);
    memmove(outdata + data_offset + 4, global->local_client_id, 4);
    memintcopy_lt(outdata + data_offset + 8, global->connection_id);
    memmove(outdata + data_offset + 12, data, datalength);
    ss_sha1_hmac(hash, outdata, out_size - OBFS_HMAC_SHA1_LEN, server->iv, (int)server->iv_len, server->key, (int)server->key_len);
    memcpy(outdata + out_size - OBFS_HMAC_SHA1_LEN, hash, OBFS_HMAC_SHA1_LEN);
    return out_size;
}

int
auth_sha1_v4_client_pre_encrypt(struct obfs_t *obfs, char **pplaindata, int datalength, size_t* capacity)
{
    char *plaindata = *pplaindata;
    auth_simple_local_data *local = (auth_simple_local_data*)obfs->l_data;
    char * out_buffer = (char*)malloc((size_t)(datalength * 2 + (SSR_BUFF_SIZE * 2)));
    char * buffer = out_buffer;
    char * data = plaindata;
    int len = datalength;
    int pack_len;
    if (len > 0 && local->has_sent_header == 0) {
        int head_size = get_head_size((const uint8_t *)plaindata, datalength, 30);
        if (head_size > datalength) {
            head_size = datalength;
        }
        pack_len = auth_sha1_v4_pack_auth_data((auth_simple_global_data *)obfs->server.g_data, &obfs->server, data, head_size, buffer);
        buffer += pack_len;
        data += head_size;
        len -= head_size;
        local->has_sent_header = 1;
    }
    while ( len > auth_simple_pack_unit_size ) {
        pack_len = auth_sha1_v4_pack_data(data, auth_simple_pack_unit_size, buffer);
        buffer += pack_len;
        data += auth_simple_pack_unit_size;
        len -= auth_simple_pack_unit_size;
    }
    if (len > 0) {
        pack_len = auth_sha1_v4_pack_data(data, len, buffer);
        buffer += pack_len;
    }
    len = (int)(buffer - out_buffer);
    if ((int)*capacity < len) {
        *pplaindata = (char*)realloc(*pplaindata, *capacity = (size_t)(len * 2));
        plaindata = *pplaindata;
    }
    memmove(plaindata, out_buffer, len);
    free(out_buffer);
    return len;
}

ssize_t
auth_sha1_v4_client_post_decrypt(struct obfs_t *obfs, char **pplaindata, int datalength, size_t* capacity)
{
    int len;
    char error;
    char * buffer;
    char * out_buffer;
    char *plaindata = *pplaindata;
    auth_simple_local_data *local = (auth_simple_local_data*)obfs->l_data;
    uint8_t * recv_buffer = (uint8_t *)local->recv_buffer->buffer;
    if (local->recv_buffer->len + datalength > 16384) {
        return -1;
    }
    memmove(recv_buffer + local->recv_buffer->len, plaindata, datalength);
    local->recv_buffer->len += datalength;

    out_buffer = (char*)malloc((size_t)local->recv_buffer->len);
    buffer = out_buffer;
    error = 0;
    while (local->recv_buffer->len > 4) {
        int length;
        int pos;
        int data_size;
        uint32_t crc_val = crc32_imp((unsigned char*)recv_buffer, 2);
        if ((((uint32_t)recv_buffer[3] << 8) | recv_buffer[2]) != (crc_val & 0xffff)) {
            local->recv_buffer->len = 0;
            error = 1;
            break;
        }
        length = ((int)recv_buffer[0] << 8) | recv_buffer[1];
        if (length >= 8192 || length < 7) {
            local->recv_buffer->len = 0;
            error = 1;
            break;
        }
        if (length > local->recv_buffer->len) {
            break;
        }
        if (checkadler32((unsigned char*)recv_buffer, (unsigned int)length) == 0) {
            local->recv_buffer->len = 0;
            error = 1;
            break;
        }
        pos = recv_buffer[4];
        if (pos < 255) {
            pos += 4;
        } else {
            pos = (((int)recv_buffer[5] << 8) | recv_buffer[6]) + 4;
        }
        data_size = length - pos - 4;
        memmove(buffer, recv_buffer + pos, data_size);
        buffer += data_size;
        memmove(recv_buffer, recv_buffer + length, local->recv_buffer->len -= length);
    }
    if (error == 0) {
        len = (int)(buffer - out_buffer);
        if ((int)*capacity < len) {
            *pplaindata = (char*)realloc(*pplaindata, *capacity = (size_t)(len * 2));
            plaindata = *pplaindata;
        }
        memmove(plaindata, out_buffer, len);
    } else {
        len = -1;
    }
    free(out_buffer);
    return (ssize_t)len;
}

unsigned int
get_rand_len(int datalength, int fulldatalength, auth_simple_local_data *local, struct server_info_t *server)
{
    if (datalength > 1300 || local->last_data_len > 1300 || fulldatalength >= (int)server->buffer_size) {
        return 0;
    }
    if (datalength > 1100) {
        return xorshift128plus() & 0x7F;
    }
    if (datalength > 900) {
        return xorshift128plus() & 0xFF;
    }
    if (datalength > 400) {
        return xorshift128plus() & 0x1FF;
    }
    return xorshift128plus() & 0x3FF;
}

int
auth_aes128_sha1_pack_data(char *data, int datalength, int fulldatalength, char *outdata, struct obfs_t *obfs)
{
    auth_simple_local_data *local = (auth_simple_local_data*)obfs->l_data;
    struct server_info_t *server = &obfs->server;
    uint8_t key_len;
    uint8_t *key;
    unsigned int rand_len = get_rand_len(datalength, fulldatalength, local, server) + 1;
    int out_size = (int)rand_len + datalength + 8;
    memcpy(outdata + rand_len + 4, data, datalength);
    outdata[0] = (char)out_size;
    outdata[1] = (char)(out_size >> 8);
    key_len = (uint8_t)(local->user_key_len + 4);
    key = (uint8_t*)malloc(key_len);
    memcpy(key, local->user_key, local->user_key_len);
    memintcopy_lt(key + key_len - 4, local->pack_id);

    {
        uint8_t * rnd_data = (uint8_t *) malloc(rand_len * sizeof(uint8_t));
        rand_bytes(rnd_data, (int)rand_len);
        memcpy(outdata + 4, rnd_data, rand_len);
        free(rnd_data);
    }

    {
        char hash[20];
        local->hmac(hash, outdata, 2, key, key_len);
        memcpy(outdata + 2, hash, 2);
    }

    if (rand_len < 128) {
        outdata[4] = (char)rand_len;
    } else {
        outdata[4] = (char)0xFF;
        outdata[5] = (char)rand_len;
        outdata[6] = (char)(rand_len >> 8);
    }
    ++local->pack_id;

    {
        char hash[20];
        local->hmac(hash, outdata, out_size - 4, key, key_len);
        memcpy(outdata + out_size - 4, hash, 4);
    }
    free(key);

    return out_size;
}

int
auth_aes128_sha1_pack_auth_data(auth_simple_global_data *global, struct server_info_t *server, auth_simple_local_data *local, char *data, int datalength, char *outdata)
{
    time_t t;
    unsigned int rand_len = (datalength > 400 ? (xorshift128plus() & 0x1FF) : (xorshift128plus() & 0x3FF));
    int data_offset = (int)rand_len + 16 + 4 + 4 + 7;
    int out_size = data_offset + datalength + 4;
    const char* salt = local->salt;

    char encrypt[24];
    char encrypt_data[16];

    uint8_t *key = (uint8_t*)malloc(server->iv_len + server->key_len);
    uint8_t key_len = (uint8_t)(server->iv_len + server->key_len);
    memcpy(key, server->iv, server->iv_len);
    memcpy(key + server->iv_len, server->key, server->key_len);

    {
        uint8_t *rnd_data = (uint8_t *) malloc(rand_len * sizeof(uint8_t));
        rand_bytes(rnd_data, (int)rand_len);
        memcpy(outdata + data_offset - rand_len, rnd_data, rand_len);
        free(rnd_data);
    }

    ++global->connection_id;
    if (global->connection_id > 0xFF000000) {
        rand_bytes(global->local_client_id, 8);
        rand_bytes((uint8_t*)&global->connection_id, 4);
        global->connection_id &= 0xFFFFFF;
    }
    t = time(NULL);
    memintcopy_lt(encrypt, (uint32_t)t);
    memcpy(encrypt + 4, global->local_client_id, 4);
    memintcopy_lt(encrypt + 8, global->connection_id);
    encrypt[12] = (char)out_size;
    encrypt[13] = (char)(out_size >> 8);
    encrypt[14] = (char)rand_len;
    encrypt[15] = (char)(rand_len >> 8);

    {
        int enc_key_len;
        char enc_key[16];
        int base64_len;
        char encrypt_key_base64[256] = {0};
        unsigned char *encrypt_key;
        if (local->user_key == NULL) {
            if(server->param != NULL && server->param[0] != 0) {
                char *param = server->param;
                char *delim = strchr(param, ':');
                if(delim != NULL) {
                    char hash[21] = {0};
                    long uid_long;
                    char key_str[128];
                    char uid_str[16] = { 0 };
                    strncpy(uid_str, param, delim - param);
                    strcpy(key_str, delim + 1);
                    uid_long = strtol(uid_str, NULL, 10);
                    memintcopy_lt(local->uid, (uint32_t)uid_long);

                    local->hash(hash, key_str, (int)strlen(key_str));

                    local->user_key_len = local->hash_len;
                    local->user_key = (uint8_t*)malloc((size_t)local->user_key_len);
                    memcpy(local->user_key, hash, local->hash_len);
                }
            }
            if (local->user_key == NULL) {
                rand_bytes((uint8_t *)local->uid, 4);

                local->user_key_len = (int)server->key_len;
                local->user_key = (uint8_t*)malloc((size_t)local->user_key_len);
                memcpy(local->user_key, server->key, local->user_key_len);
            }
        }

        encrypt_key = (unsigned char *) malloc((size_t)local->user_key_len * sizeof(unsigned char));
        memcpy(encrypt_key, local->user_key, local->user_key_len);
        std_base64_encode(encrypt_key, local->user_key_len, (unsigned char *)encrypt_key_base64);
        free(encrypt_key);

        base64_len = (local->user_key_len + 2) / 3 * 4;
        memcpy(encrypt_key_base64 + base64_len, salt, strlen(salt));

        enc_key_len = base64_len + (int)strlen(salt);
        bytes_to_key_with_size(encrypt_key_base64, (size_t)enc_key_len, (uint8_t*)enc_key, 16);
        ss_aes_128_cbc(encrypt, encrypt_data, enc_key);
        memcpy(encrypt + 4, encrypt_data, 16);
        memcpy(encrypt, local->uid, 4);
    }

    {
        char hash[20];
        local->hmac(hash, encrypt, 20, key, key_len);
        memcpy(encrypt + 20, hash, 4);
    }

    {
        char hash[20];
        rand_bytes((uint8_t*)outdata, 1);
        local->hmac(hash, (char *)outdata, 1, key, key_len);
        memcpy(outdata + 1, hash, 6);
    }

    memcpy(outdata + 7, encrypt, 24);
    memcpy(outdata + data_offset, data, datalength);

    {
        char hash[20];
        local->hmac(hash, outdata, out_size - 4, local->user_key, local->user_key_len);
        memmove(outdata + out_size - 4, hash, 4);
    }
    free(key);

    return out_size;
}

int
auth_aes128_sha1_client_pre_encrypt(struct obfs_t *obfs, char **pplaindata, int datalength, size_t* capacity)
{
    char *plaindata = *pplaindata;
    auth_simple_local_data *local = (auth_simple_local_data*)obfs->l_data;
    char * out_buffer = (char*)calloc((size_t)(datalength * 2 + (SSR_BUFF_SIZE * 2)), sizeof(char));
    char * buffer = out_buffer;
    char * data = plaindata;
    int len = datalength;
    int pack_len;
    if (len > 0 && local->has_sent_header == 0) {
        int head_size = 1200;
        if (head_size > datalength) {
            head_size = datalength;
        }
        pack_len = auth_aes128_sha1_pack_auth_data((auth_simple_global_data *)obfs->server.g_data, &obfs->server, local, data, head_size, buffer);
        buffer += pack_len;
        data += head_size;
        len -= head_size;
        local->has_sent_header = 1;
    }
    while ( len > auth_simple_pack_unit_size ) {
        pack_len = auth_aes128_sha1_pack_data(data, auth_simple_pack_unit_size, datalength, buffer, obfs);
        buffer += pack_len;
        data += auth_simple_pack_unit_size;
        len -= auth_simple_pack_unit_size;
    }
    if (len > 0) {
        pack_len = auth_aes128_sha1_pack_data(data, len, datalength, buffer, obfs);
        buffer += pack_len;
    }
    len = (int)(buffer - out_buffer);
    if ((int)*capacity < len) {
        *pplaindata = (char*)realloc(*pplaindata, *capacity = (size_t)(len * 2));
        plaindata = *pplaindata;
    }
    local->last_data_len = datalength;
    memmove(plaindata, out_buffer, len);
    free(out_buffer);
    return len;
}

ssize_t
auth_aes128_sha1_client_post_decrypt(struct obfs_t *obfs, char **pplaindata, int datalength, size_t* capacity)
{
    int len;
    int key_len;
    uint8_t *key;
    char * out_buffer;
    char * buffer;
    char error = 0;
    char *plaindata = *pplaindata;
    auth_simple_local_data *local = (auth_simple_local_data*)obfs->l_data;
    //struct server_info_t *server = (struct server_info_t *)&obfs->server;
    uint8_t * recv_buffer = (uint8_t *)local->recv_buffer->buffer;
    if (local->recv_buffer->len + datalength > 16384) {
        return -1;
    }
    memmove(recv_buffer + local->recv_buffer->len, plaindata, datalength);
    local->recv_buffer->len += datalength;

    key_len = local->user_key_len + 4;
    key = (uint8_t*)malloc((size_t)key_len);
    memcpy(key, local->user_key, local->user_key_len);

    out_buffer = (char*)malloc((size_t)local->recv_buffer->len);
    buffer = out_buffer;
    while (local->recv_buffer->len > 4) {
        int length;
        int pos;
        int data_size;
        memintcopy_lt(key + key_len - 4, local->recv_id);

        {
            char hash[20];
            local->hmac(hash, (char*)recv_buffer, 2, key, key_len);

            if (memcmp(hash, recv_buffer + 2, 2)) {
                local->recv_buffer->len = 0;
                error = 1;
                break;
            }
        }

        length = ((int)recv_buffer[1] << 8) + recv_buffer[0];
        if (length >= 8192 || length < 8) {
            local->recv_buffer->len = 0;
            error = 1;
            break;
        }
        if (length > local->recv_buffer->len) {
            break;
        }

        {
            char hash[20];
            local->hmac(hash, (char *)recv_buffer, length - 4, key, key_len);
            if (memcmp(hash, recv_buffer + length - 4, 4)) {
                local->recv_buffer->len = 0;
                error = 1;
                break;
            }
        }

        ++local->recv_id;
        pos = recv_buffer[4];
        if (pos < 255) {
            pos += 4;
        } else {
            pos = (((int)recv_buffer[6] << 8) | recv_buffer[5]) + 4;
        }
        data_size = length - pos - 4;
        memmove(buffer, recv_buffer + pos, data_size);
        buffer += data_size;
        memmove(recv_buffer, recv_buffer + length, local->recv_buffer->len -= length);
    }
    if (error == 0) {
        len = (int)(buffer - out_buffer);
        if ((int)*capacity < len) {
            *pplaindata = (char*)realloc(*pplaindata, *capacity = (size_t)(len * 2));
            plaindata = *pplaindata;
        }
        memmove(plaindata, out_buffer, len);
    } else {
        len = -1;
    }
    free(out_buffer);
    free(key);
    return (ssize_t)len;
}

ssize_t
auth_aes128_sha1_client_udp_pre_encrypt(struct obfs_t *obfs, char **pplaindata, size_t datalength, size_t* capacity)
{
    size_t outlength;
    char *plaindata = *pplaindata;
    auth_simple_local_data *local = (auth_simple_local_data*)obfs->l_data;
    char * out_buffer = (char*)malloc((datalength + 8));

    if (local->user_key == NULL) {
        if(obfs->server.param != NULL && obfs->server.param[0] != 0) {
            char *param = obfs->server.param;
            char *delim = strchr(param, ':');
            if(delim != NULL) {
                char key_str[128];
                long uid_long;
                char hash[21] = {0};
                char uid_str[16] = { 0 };

                strncpy(uid_str, param, delim - param);
                strcpy(key_str, delim + 1);
                uid_long = strtol(uid_str, NULL, 10);
                memintcopy_lt(local->uid, (uint32_t)uid_long);

                local->hash(hash, key_str, (int)strlen(key_str));

                local->user_key_len = local->hash_len;
                local->user_key = (uint8_t*)malloc((size_t)local->user_key_len);
                memcpy(local->user_key, hash, local->hash_len);
            }
        }
        if (local->user_key == NULL) {
            rand_bytes((uint8_t *)local->uid, 4);

            local->user_key_len = (int)obfs->server.key_len;
            local->user_key = (uint8_t*)malloc((size_t)local->user_key_len);
            memcpy(local->user_key, obfs->server.key, local->user_key_len);
        }
    }

    outlength = datalength + 8;
    memmove(out_buffer, plaindata, datalength);
    memmove(out_buffer + datalength, local->uid, 4);

    {
        char hash[20];
        local->hmac(hash, out_buffer, (int)(outlength - 4), local->user_key, local->user_key_len);
        memmove(out_buffer + outlength - 4, hash, 4);
    }

    if (*capacity < outlength) {
        *pplaindata = (char*)realloc(*pplaindata, *capacity = (outlength * 2));
        plaindata = *pplaindata;
    }
    memmove(plaindata, out_buffer, outlength);
    free(out_buffer);
    return (ssize_t)outlength;
}

ssize_t
auth_aes128_sha1_client_udp_post_decrypt(struct obfs_t *obfs, char **pplaindata, size_t datalength, size_t* capacity)
{
    char *plaindata;
    auth_simple_local_data *local;
    char hash[20];

    if (datalength <= 4) {
        return 0;
    }
    plaindata = *pplaindata;
    local = (auth_simple_local_data*)obfs->l_data;

    local->hmac(hash, plaindata, (int)(datalength - 4), obfs->server.key, (int)obfs->server.key_len);

    if (memcmp(hash, plaindata + datalength - 4, 4)) {
        return 0;
    }

    return (ssize_t)(datalength - 4);
}

struct buffer_t * auth_aes128_sha1_server_pre_encrypt(struct obfs_t *obfs, struct buffer_t *buf) {
    // TODO : need implementation future.
    return generic_server_pre_encrypt(obfs, buf);
}

struct buffer_t * auth_aes128_sha1_server_encode(struct obfs_t *obfs, struct buffer_t *buf) {
    // TODO : need implementation future.
    return generic_server_encode(obfs, buf);
}

struct buffer_t * auth_aes128_sha1_server_decode(struct obfs_t *obfs, const struct buffer_t *buf, bool *need_decrypt, bool *need_feedback) {
    // TODO : need implementation future.
    return generic_server_decode(obfs, buf, need_decrypt, need_feedback);
}

struct buffer_t * auth_aes128_sha1_server_post_decrypt(struct obfs_t *obfs, struct buffer_t *buf, bool *need_feedback) {
    // TODO : need implementation future.
    return generic_server_post_decrypt(obfs, buf, need_feedback);
}

bool auth_aes128_sha1_server_udp_pre_encrypt(struct obfs_t *obfs, struct buffer_t *buf) {
    // TODO : need implementation future.
    return generic_server_udp_pre_encrypt(obfs, buf);
}

bool auth_aes128_sha1_server_udp_post_decrypt(struct obfs_t *obfs, struct buffer_t *buf, uint32_t *uid) {
    // TODO : need implementation future.
    return generic_server_udp_post_decrypt(obfs, buf, uid);
}
