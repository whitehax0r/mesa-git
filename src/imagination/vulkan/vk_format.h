/*
 * Copyright © 2022 Imagination Technologies Ltd.
 *
 * based in part on radv driver which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * Based on u_format.h which is:
 * Copyright 2009-2010 VMware, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* clang-format off */
#ifndef VK_FORMAT_H
#define VK_FORMAT_H

#include <stdbool.h>
#include <util/format/u_format.h>
#include <vulkan/util/vk_format.h>

#include <vulkan/vulkan.h>

#include "util/u_endian.h"

static inline bool
vk_format_is_alpha_on_msb(VkFormat vk_format)
{
   const struct util_format_description *desc =
      vk_format_description(vk_format);

   return (desc->colorspace == UTIL_FORMAT_COLORSPACE_RGB ||
           desc->colorspace == UTIL_FORMAT_COLORSPACE_SRGB) &&
#if defined(UTIL_ARCH_BIG_ENDIAN)
           desc->swizzle[3] == PIPE_SWIZZLE_X;
#else
           desc->swizzle[3] == PIPE_SWIZZLE_W;
#endif
}

static inline bool
vk_format_is_pure_integer(VkFormat vk_format)
{
   return util_format_is_pure_integer(vk_format_to_pipe_format(vk_format));
}

static inline uint
vk_format_get_channel_width(VkFormat vk_format, uint32_t channel)
{
   const struct util_format_description *desc =
      vk_format_description(vk_format);

   return desc->channel[channel].size;
}

static inline bool
vk_format_has_32bit_component(VkFormat vk_format)
{
   const struct util_format_description *desc =
      vk_format_description(vk_format);

   for (uint32_t i = 0; i < desc->nr_channels; i++) {
      if (desc->channel[i].size == 32U)
         return true;
   }

   return false;
}

static inline uint
vk_format_get_component_bits(VkFormat vk_format,
                             enum util_format_colorspace colorspace,
                             uint32_t component)
{
   return util_format_get_component_bits(vk_format_to_pipe_format(vk_format),
                                         colorspace,
                                         component);
}

static inline bool
vk_format_is_normalized(VkFormat vk_format)
{
   const struct util_format_description *desc =
      vk_format_description(vk_format);

   for (uint32_t i = 0; i < desc->nr_channels; i++) {
      if (!desc->channel[i].normalized)
         return false;
   }

   return true;
}

#endif /* VK_FORMAT_H */
