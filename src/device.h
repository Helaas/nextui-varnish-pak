/*
 * device.h — Platform detection, path resolution, and device metrics.
 */

#ifndef VARNISH_DEVICE_H
#define VARNISH_DEVICE_H

#include <stddef.h>

void device_get_sdcard_path(char *out, size_t size);
void device_get_platform_name(char *out, size_t size);
void device_get_userdata_path(char *out, size_t size);
void device_get_shared_userdata_path(char *out, size_t size);
void device_get_system_bin_path(char *out, size_t size);
void device_get_pak_dir(char *out, size_t size);
int  device_get_fb_dimensions(int *out_w, int *out_h);

/* Device metrics for scaling */
int  device_get_scale(int fb_width, int fb_height);
int  device_get_padding(int fb_width, int fb_height);

#endif /* VARNISH_DEVICE_H */
