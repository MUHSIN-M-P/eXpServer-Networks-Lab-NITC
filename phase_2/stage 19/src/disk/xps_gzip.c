#include "xps_gzip.h"

xps_gzip_t *xps_gzip_create(int level)
{
    if (level < -1 || level > 9)
    {
        logger(LOG_ERROR, "xps_gzip_create()", "invalid compression level %d", level);
        return NULL;
    }

    xps_gzip_t *gzip = (xps_gzip_t *)malloc(sizeof(xps_gzip_t));
    if (!gzip)
    {
        logger(LOG_ERROR, "xps_gzip_create()", "failed to allocate memory for xps_gzip_t");
        return NULL;
    }
    // Initialize the stream object
    gzip->stream.zalloc = Z_NULL;
    gzip->stream.zfree = Z_NULL;
    gzip->stream.opaque = Z_NULL;

    // Create the source that will provide compressed output.
    gzip->source = xps_pipe_source_create((void *)gzip, gzip_source_handler, gzip_source_close_handler);
    if (!gzip->source)
    {
        logger(LOG_ERROR, "xps_gzip_create()", "failed to create pipe source");
        free(gzip);
        return NULL;
    }

    // Create the sink that will receive uncompressed input.
    gzip->sink = xps_pipe_sink_create((void *)gzip, gzip_sink_handler, gzip_sink_close_handler);
    if (!gzip->sink)
    {
        logger(LOG_ERROR, "xps_gzip_create()", "failed to create pipe sink");
        xps_pipe_source_destroy(gzip->source);
        free(gzip);
        return NULL;
    }

    // Initialize zlib compression stream
    if (deflateInit2(&(gzip->stream), level, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
    {
        logger(LOG_ERROR, "xps_gzip_create()", "deflateInit2() failed");
        xps_pipe_source_destroy(gzip->source);
        xps_pipe_sink_destroy(gzip->sink);
        free(gzip);
        return NULL;
    }

    // gzip module is ready to receive its first chunk of data. ready
    gzip->sink->ready = true;
    gzip->transfer_buff = NULL;
    gzip->total_in_len = 0;
    gzip->total_out_len = 0;

    return gzip;
}

void xps_gzip_destroy(xps_gzip_t *gzip)
{
    assert(gzip != NULL);

    deflateEnd(&(gzip->stream));
    xps_pipe_source_destroy(gzip->source);
    xps_pipe_sink_destroy(gzip->sink);

    if (gzip->transfer_buff)
        xps_buffer_destroy(gzip->transfer_buff);

    free(gzip);
}

void gzip_compress(xps_gzip_t *gzip, bool flush)
{
    assert(gzip != NULL);

    xps_buffer_t *in_buff = NULL;

    if (!flush)
    {
        // Read data from the sink's pipe buffer list
        size_t input_len = gzip->sink->pipe->buff_list->len;

        if (input_len == 0)
            return;

        in_buff = xps_pipe_sink_read(gzip->sink, input_len);
        if (!in_buff)
        {
            logger(LOG_ERROR, "gzip_compress()", "xps_pipe_sink_read() failed");
            return;
        }
    }
    else
    {
        // Create An empty buffer so that the function can perform the final deflate() operation.
        in_buff = xps_buffer_create(1, 0, NULL);
        if (!in_buff)
        {
            logger(LOG_ERROR, "gzip_compress()", "xps_buffer_create() failed");
            return;
        }
    }

    size_t input_len = in_buff->len;

    // Configure the zlib stream for compression
    gzip->stream.avail_in = input_len;
    gzip->stream.next_in = in_buff->data;

    int flush_status = flush ? Z_FINISH : Z_NO_FLUSH;

    // A buffer list is used to collect the compressed output because one call to deflate()
    // may produce more compressed data than can fit in a single output buffer.
    xps_buffer_list_t *out_buff_list = xps_buffer_list_create();
    if (!out_buff_list)
    {
        logger(LOG_ERROR, "gzip_compress()", "xps_buffer_list_create() failed");
        xps_buffer_destroy(in_buff);
        return;
    }

    int error;

    do
    {
        // Create output buffer
        xps_buffer_t *out_buff = xps_buffer_create(DEFAULT_BUFFER_SIZE, 0, NULL);
        if (!out_buff)
        {
            logger(LOG_ERROR, "gzip_compress()", "xps_buffer_create() failed");
            xps_buffer_destroy(in_buff);
            xps_buffer_list_destroy(out_buff_list);
            return;
        }

        // Configure the zlib stream for output
        gzip->stream.avail_out = out_buff->size;
        gzip->stream.next_out = out_buff->data;

        // Compress the data
        error = deflate(&(gzip->stream), flush_status);

        if (error != Z_OK && error != Z_STREAM_END)
        {
            logger(LOG_ERROR, "gzip_compress()", "deflate() failed with code %d", error);
            xps_buffer_destroy(out_buff);
            xps_buffer_destroy(in_buff);
            xps_buffer_list_destroy(out_buff_list);
            return;
        }

        // Update the length of the output buffer based on how much data was written
        out_buff->len = out_buff->size - gzip->stream.avail_out;

        // Append the output buffer to the output buffer list if it contains data
        if (out_buff->len > 0)
            xps_buffer_list_append(out_buff_list, out_buff);
        else
            xps_buffer_destroy(out_buff);
    } while (gzip->stream.avail_out == 0 || (flush && error != Z_STREAM_END));
    assert(gzip->stream.avail_in == 0);

    xps_buffer_t *compressed_buff = NULL;

    // Once all available input has been consumed, If the output buffer list contains data, read it into a single buffer
    if (out_buff_list->len > 0)
    {
        compressed_buff = xps_buffer_list_read(out_buff_list, out_buff_list->len);
        if (!compressed_buff)
        {
            logger(LOG_ERROR, "gzip_compress()", "xps_buffer_list_read() failed");
            xps_buffer_destroy(in_buff);
            xps_buffer_list_destroy(out_buff_list);
            return;
        }
    }

    xps_buffer_list_destroy(out_buff_list);

    if (!flush)
    {
        // Clear the input buffer from the sink's pipe buffer list
        if (xps_pipe_sink_clear(gzip->sink, input_len) != OK)
        {
            logger(LOG_ERROR, "gzip_compress()", "xps_pipe_sink_clear() failed");
            xps_buffer_destroy(in_buff);
            if (compressed_buff)
                xps_buffer_destroy(compressed_buff);
            return;
        }
        gzip->total_in_len += input_len;
    }

    // Destroy the input buffer
    xps_buffer_destroy(in_buff);

    // Update transfer buffer and ready status
    gzip->transfer_buff = compressed_buff;
    gzip->source->ready = (compressed_buff != NULL);
    gzip->sink->ready = (compressed_buff == NULL);
}

int sink_handler_calls = 0;
int source_handler_calls = 0;
void gzip_sink_handler(void *ptr)
{
    sink_handler_calls++;
    // printf("gzip_sink_handler() called %d times\n", sink_handler_calls);
    xps_pipe_sink_t *sink = ptr;
    xps_gzip_t *gzip = sink->ptr;
    gzip_compress(gzip, false);
}

void gzip_sink_close_handler(void *ptr)
{
    xps_pipe_sink_t *sink = ptr;
    xps_gzip_t *gzip = sink->ptr;
    gzip_compress(gzip, true);
}

void gzip_source_handler(void *ptr)
{
    source_handler_calls++;
    // printf("gzip_source_handler() called %d times\n", source_handler_calls);

    xps_pipe_source_t *source = (xps_pipe_source_t *)ptr;
    xps_gzip_t *gzip = (xps_gzip_t *)source->ptr;

    // Write compressed buffer to downstream pipe
    if (xps_pipe_source_write(gzip->source, gzip->transfer_buff) != OK)
    {
        logger(LOG_ERROR, "gzip_source_handler()", "xps_pipe_source_write() failed");
        return;
    }

    gzip->total_out_len += gzip->transfer_buff->len;

    // Destroy transfer buffer
    xps_buffer_destroy(gzip->transfer_buff);
    gzip->transfer_buff = NULL;

    gzip->source->ready = false;
    gzip->sink->ready = true;

    gzip_check_destroy(gzip);
}

void gzip_source_close_handler(void *ptr)
{
    xps_pipe_source_t *source = (xps_pipe_source_t *)ptr;
    xps_gzip_t *gzip = (xps_gzip_t *)source->ptr;

    gzip_check_destroy(gzip);
}

void gzip_check_destroy(xps_gzip_t *gzip)
{
    assert(gzip != NULL);

    // destroy if:

    // - downstream is closed, OR

    // - upstream is closed AND no pending dat
    if (!gzip->source->active || (!gzip->sink->active && gzip->transfer_buff == NULL))
    {
        xps_gzip_destroy(gzip);
    }
}