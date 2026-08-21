#include "wav_loader.h"
#include "../Debug/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Helper struct for parsing
typedef struct {
    char id[4];
    unsigned int size;
} chunk_header;

static inline void skip_chunk_payload(FILE* f, unsigned int size) {
    long skip = (long)size;
    if (size & 1u) skip += 1;
    fseek(f, skip, SEEK_CUR);
}

wav_data* wav_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        logger_log("Failed to open WAV file: %s\n", path);
        return NULL;
    }

    // Read RIFF header
    chunk_header riff_header;
    char wave_id[4];
    if (fread(&riff_header, sizeof(chunk_header), 1, f) != 1 || strncmp(riff_header.id, "RIFF", 4) != 0) {
        logger_log("Not a RIFF file: %s\n", path);
        fclose(f);
        return NULL;
    }
    if (fread(wave_id, 1, 4, f) != 4 || strncmp(wave_id, "WAVE", 4) != 0) {
        logger_log("Not a WAVE file: %s\n", path);
        fclose(f);
        return NULL;
    }

    wav_data* data = (wav_data*)calloc(1, sizeof(wav_data));
    if (!data) {
        logger_log("Failed to allocate memory for wav_data: %s\n", path);
        fclose(f);
        return NULL;
    }
    data->root_key = -1;
    data->root_key_fraction = 0.0f;
    data->has_loop = 0;
    data->loop_start = 0;
    data->loop_end = 0;

    short bits_per_sample = 0;
    int found_fmt = 0;
    int found_data = 0;

    // Loop through chunks
    chunk_header chunk;
    while (fread(&chunk, sizeof(chunk_header), 1, f) == 1) {
        if (strncmp(chunk.id, "fmt ", 4) == 0) {
            found_fmt = 1;
            // We only support 16-bit PCM for now
            short format_tag;
            if (fread(&format_tag, 2, 1, f) != 1 ||
                fread(&data->num_channels, 2, 1, f) != 1 ||
                fread(&data->sample_rate, 4, 1, f) != 1) {
                logger_log("Failed to read WAV fmt chunk: %s\n", path);
                wav_free(data);
                fclose(f);
                return NULL;
            }
            fseek(f, 6, SEEK_CUR); // Skip avg_bytes_per_sec and block_align
            if (fread(&bits_per_sample, 2, 1, f) != 1) {
                logger_log("Failed to read WAV bits_per_sample: %s\n", path);
                wav_free(data);
                fclose(f);
                return NULL;
            }

            if (format_tag != 1 || bits_per_sample != 16) {
                logger_log("Unsupported WAV format (not 16-bit PCM): %s\n", path);
                wav_free(data);
                fclose(f);
                return NULL;
            }
            
            // Skip rest of fmt chunk if it's larger than the standard 16 bytes
            if (chunk.size > 16) {
                fseek(f, chunk.size - 16, SEEK_CUR);
            }
            if (chunk.size & 1u) {
                fseek(f, 1, SEEK_CUR);
            }

        } else if (strncmp(chunk.id, "data", 4) == 0) {
            found_data = 1;
            if (!found_fmt) {
                logger_log("Found 'data' chunk before 'fmt ' in: %s\n", path);
                wav_free(data);
                fclose(f);
                return NULL;
            }
            
            data->num_samples = chunk.size / (data->num_channels * (bits_per_sample / 8));
            data->data = (short*)malloc(chunk.size);
            if (!data->data) {
                logger_log("Failed to allocate memory for sample data: %s\n", path);
                wav_free(data);
                fclose(f);
                return NULL;
            }

            if (fread(data->data, 1, chunk.size, f) != chunk.size) {
                logger_log("Failed to read sample data from: %s\n", path);
                wav_free(data); // This will free data->data as well
                fclose(f);
                return NULL;
            }
            if (chunk.size & 1u) {
                fseek(f, 1, SEEK_CUR);
            }
        } else if (strncmp(chunk.id, "smpl", 4) == 0) {
            if (chunk.size >= 36) {
                uint32_t smpl_fields[9];
                if (fread(smpl_fields, sizeof(uint32_t), 9, f) == 9) {
                    uint32_t unity_note = smpl_fields[3];
                    uint32_t pitch_fraction = smpl_fields[4];
                    uint32_t num_loops = smpl_fields[7];
                    if (unity_note <= 127u) {
                        data->root_key = (int)unity_note;
                    }
                    data->root_key_fraction = (float)((double)pitch_fraction / 4294967296.0);
                    if (num_loops > 0 && chunk.size >= 60) {
                        uint32_t loop_fields[6];
                        if (fread(loop_fields, sizeof(uint32_t), 6, f) == 6) {
                            data->has_loop = 1;
                            data->loop_start = (int)loop_fields[2];
                            data->loop_end = (int)loop_fields[3];
                        }
                        if (chunk.size > 60) {
                            fseek(f, (long)(chunk.size - 60), SEEK_CUR);
                        }
                    } else if (chunk.size > 36) {
                        fseek(f, (long)(chunk.size - 36), SEEK_CUR);
                    }
                } else {
                    logger_log("Failed to read WAV smpl chunk: %s\n", path);
                }
                if (chunk.size & 1u) {
                    fseek(f, 1, SEEK_CUR);
                }
            } else {
                skip_chunk_payload(f, chunk.size);
            }

        } else {
            // Skip unknown chunks
            skip_chunk_payload(f, chunk.size);
        }
    }

    fclose(f);

    if (!found_fmt || !found_data) {
        logger_log("WAV file missing 'fmt ' or 'data' chunk: %s\n", path);
        wav_free(data);
        return NULL;
    }

    return data;
}

void wav_free(wav_data* data) {
    if (data) {
        if (data->data) {
            free(data->data);
        }
        free(data);
    }
}
