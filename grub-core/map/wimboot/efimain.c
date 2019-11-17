 /*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2019  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <grub/dl.h>
#include <grub/efi/api.h>
#include <grub/efi/efi.h>
#include <grub/device.h>
#include <grub/err.h>
#include <grub/env.h>
#include <grub/extcmd.h>
#include <grub/file.h>
#include <grub/i18n.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/types.h>
#include <grub/term.h>

#include <maplib.h>
#include <private.h>
#include <efiapi.h>
#include <wimboot.h>
#include <vfat.h>

GRUB_MOD_LICENSE ("GPLv3+");

static const struct grub_arg_option options_wimboot[] = {
  {"gui", 'g', 0, N_("Display graphical boot messages."), 0, 0},
  {"rawbcd", 'b', 0, N_("Disable rewriting .exe to .efi in the BCD file."), 0, 0},
  {"rawwim", 'w', 0, N_("Disable patching the wim file."), 0, 0},
  {"index", 'i', 0, N_("Use WIM image index n."), N_("n"), ARG_TYPE_INT},
  {"pause", 'p', 0, N_("Show info and wait for keypress."), 0, 0},
  {"inject", 'j', 0, N_("Set inject dir."), N_("PATH"), ARG_TYPE_STRING},
  {0, 0, 0, 0, 0, 0}
};

enum options_wimboot
{
  WIMBOOT_GUI,
  WIMBOOT_RAWBCD,
  WIMBOOT_RAWWIM,
  WIMBOOT_INDEX,
  WIMBOOT_PAUSE,
  WIMBOOT_INJECT
};

struct wimboot_cmdline wimboot_cmd =
{
  FALSE,
  FALSE,
  FALSE,
  0,
  FALSE,
  L"\\Windows\\System32",
};

static void
grub_wimboot_close (struct grub_wimboot_context *wimboot_ctx)
{
  int i;
  if (!wimboot_ctx->components)
    return;
  for (i = 0; i < wimboot_ctx->nfiles; i++)
    {
      grub_free (wimboot_ctx->components[i].file_name);
      grub_file_close (wimboot_ctx->components[i].file);
    }
  grub_free (wimboot_ctx->components);
  wimboot_ctx->components = 0;
}

static grub_err_t
grub_wimboot_init (int argc, char *argv[],
                   struct grub_wimboot_context *wimboot_ctx)
{
  int i;

  wimboot_ctx->nfiles = 0;
  wimboot_ctx->components = 0;
  wimboot_ctx->components =
          grub_zalloc (argc * sizeof (wimboot_ctx->components[0]));
  if (!wimboot_ctx->components)
    return grub_errno;

  for (i = 0; i < argc; i++)
  {
    const char *fname = argv[i];
    if (grub_memcmp (argv[i], "@:", 2) == 0)
    {
      const char *ptr, *eptr;
      ptr = argv[i] + 2;
      while (*ptr == '/')
        ptr++;
      eptr = grub_strchr (ptr, ':');
      if (eptr)
      {
        wimboot_ctx->components[i].file_name = grub_strndup (ptr, eptr - ptr);
        if (!wimboot_ctx->components[i].file_name)
        {
          grub_wimboot_close (wimboot_ctx);
          return grub_errno;
        }
        fname = eptr + 1;
      }
    }
    wimboot_ctx->components[i].file = grub_file_open (fname,
                GRUB_FILE_TYPE_LINUX_INITRD | GRUB_FILE_TYPE_NO_DECOMPRESS);
    if (!wimboot_ctx->components[i].file)
    {
      grub_wimboot_close (wimboot_ctx);
      return grub_errno;
    }
    wimboot_ctx->nfiles++;
    grub_printf ("file %d: %s path: %s\n",
                 wimboot_ctx->nfiles, wimboot_ctx->components[i].file_name, fname);
  }

  return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_wimboot (grub_extcmd_context_t ctxt,
                  int argc, char *argv[])
{
  struct grub_arg_list *state = ctxt->state;
  struct grub_wimboot_context wimboot_ctx = {0, 0};
  const char *progress = grub_env_get ("enable_progress_indicator");

  if (argc == 0)
  {
    grub_error (GRUB_ERR_BAD_ARGUMENT, N_("filename expected"));
    goto fail;
  }

  if (grub_wimboot_init (argc, argv, &wimboot_ctx))
    goto fail;

  grub_env_set ("enable_progress_indicator", "1");

  if (state[WIMBOOT_GUI].set)
    wimboot_cmd.gui = TRUE;
  if (state[WIMBOOT_RAWBCD].set)
    wimboot_cmd.rawbcd = TRUE;
  if (state[WIMBOOT_RAWWIM].set)
    wimboot_cmd.rawwim = TRUE;
  if (state[WIMBOOT_PAUSE].set)
    wimboot_cmd.pause = TRUE;
  if (state[WIMBOOT_INDEX].set)
    wimboot_cmd.index = grub_strtoul (state[WIMBOOT_INDEX].arg, NULL, 0);
  if (state[WIMBOOT_INJECT].set)
  {
    int i;
    char *p = state[WIMBOOT_INJECT].arg;
    for (i=0; i < 255; i++)
    {
      if (*p)
      {
        wimboot_cmd.inject[i] = *p;
        p++;
      }
      else
        break;
    }
    wimboot_cmd.inject[i] = 0;
  }

  grub_extract (&wimboot_ctx);
  wimboot_install ();
  wimboot_boot (bootmgfw);
  if (!progress)
    grub_env_unset ("enable_progress_indicator");
  else
    grub_env_set ("enable_progress_indicator", progress);
fail:
  grub_wimboot_close (&wimboot_ctx);
  return grub_errno;
}

static grub_extcmd_t cmd_wimboot;

GRUB_MOD_INIT(wimboot)
{
  cmd_wimboot = grub_register_extcmd ("wimboot", grub_cmd_wimboot, 0,
                    N_("[--rawbcd] [--index=n] [--pause] @:NAME:PATH"),
                    N_("Windows Imaging Format bootloader"), options_wimboot);
}

GRUB_MOD_FINI(wimboot)
{
  grub_unregister_extcmd (cmd_wimboot);
}