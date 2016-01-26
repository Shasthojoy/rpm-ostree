/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2014 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <glib-unix.h>
#include <rpm/rpmsq.h>
#include <rpm/rpmlib.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmfi.h>
#include <rpm/rpmmacro.h>
#include <rpm/rpmts.h>
#include <gio/gunixoutputstream.h>
#include <libhif.h>
#include <libhif/hif-utils.h>
#include <libhif/hif-package.h>

#include "rpmostree-hif.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-unpacker.h"
#include "rpmostree-cleanup.h"

#define RPMOSTREE_DIR_CACHE_REPOMD "repomd"
#define RPMOSTREE_DIR_CACHE_SOLV "repomd"
#define RPMOSTREE_DIR_LOCK "lock"

void
_rpmostree_reset_rpm_sighandlers (void)
{
  /* Forcibly override rpm/librepo SIGINT handlers.  We always operate
   * in a fully idempotent/atomic mode, and can be killed at any time.
   */
#if !BUILDOPT_HAVE_RPMSQ_SET_INTERRUPT_SAFETY
  signal (SIGINT, SIG_DFL);
  signal (SIGTERM, SIG_DFL);
#endif
}

HifContext *
_rpmostree_libhif_new_default (void)
{
  HifContext *hifctx;

  /* We can always be control-c'd at any time; this is new API,
   * otherwise we keep calling _rpmostree_reset_rpm_sighandlers() in
   * various places.
   */
#if BUILDOPT_HAVE_RPMSQ_SET_INTERRUPT_SAFETY
  rpmsqSetInterruptSafety (FALSE);
#endif

  hifctx = hif_context_new ();
  _rpmostree_reset_rpm_sighandlers ();
  hif_context_set_http_proxy (hifctx, g_getenv ("http_proxy"));

  hif_context_set_repo_dir (hifctx, "/etc/yum.repos.d");
  hif_context_set_cache_age (hifctx, G_MAXUINT);
  hif_context_set_cache_dir (hifctx, "/var/cache/rpm-ostree/" RPMOSTREE_DIR_CACHE_REPOMD);
  hif_context_set_solv_dir (hifctx, "/var/cache/rpm-ostree/" RPMOSTREE_DIR_CACHE_SOLV);
  hif_context_set_lock_dir (hifctx, "/run/rpm-ostree/" RPMOSTREE_DIR_LOCK);

  hif_context_set_check_disk_space (hifctx, FALSE);
  hif_context_set_check_transaction (hifctx, FALSE);
  hif_context_set_yumdb_enabled (hifctx, FALSE);

  return hifctx;
}

HifContext *
_rpmostree_libhif_new (int rpmmd_cache_dfd,
                       const char *installroot,
                       const char *repos_dir,
                       const char *const *enabled_repos,
                       GCancellable *cancellable,
                       GError **error)
{
  gboolean ret = FALSE;
  glnx_unref_object HifContext *hifctx = _rpmostree_libhif_new_default ();

  _rpmostree_libhif_set_cache_dfd (hifctx, rpmmd_cache_dfd);
  if (repos_dir)
    hif_context_set_repo_dir (hifctx, repos_dir);

  if (installroot)
    hif_context_set_install_root (hifctx, installroot);

  /* A nonsense value because repo files used as input for this tool
   * should never use it.
   */
  hif_context_set_release_ver (hifctx, "42");

  if (!_rpmostree_libhif_setup (hifctx, cancellable, error))
    goto out;

  if (enabled_repos)
    {
      _rpmostree_libhif_repos_disable_all (hifctx);
      
      { const char * const *strviter = enabled_repos;
        for (; strviter && *strviter; strviter++)
          {
            const char *reponame = *strviter;
            if (!_rpmostree_libhif_repos_enable_by_name (hifctx, reponame, error))
              goto out;
          }
      }
    }

  ret = TRUE;
 out:
  if (ret)
    return g_steal_pointer (&hifctx);
  return NULL;
}

void
_rpmostree_libhif_set_cache_dfd (HifContext *hifctx, int dfd)
{
  g_autofree char *repomddir =
    glnx_fdrel_abspath (dfd, RPMOSTREE_DIR_CACHE_REPOMD);
  g_autofree char *solvdir =
    glnx_fdrel_abspath (dfd, RPMOSTREE_DIR_CACHE_SOLV);
  g_autofree char *lockdir =
    glnx_fdrel_abspath (dfd, RPMOSTREE_DIR_LOCK);

  hif_context_set_cache_dir (hifctx, repomddir);
  hif_context_set_solv_dir (hifctx, solvdir);
  hif_context_set_lock_dir (hifctx, lockdir);
}

gboolean
_rpmostree_libhif_setup (HifContext    *context,
                         GCancellable  *cancellable,
                         GError       **error)
{
  if (!hif_context_setup (context, cancellable, error))
    return FALSE;

  return TRUE;
}

void
_rpmostree_libhif_repos_disable_all (HifContext    *context)
{
  GPtrArray *sources;
  guint i;

  sources = hif_context_get_sources (context);
  for (i = 0; i < sources->len; i++)
    {
      HifSource *src = sources->pdata[i];
      
      hif_source_set_enabled (src, HIF_SOURCE_ENABLED_NONE);
    }
}

gboolean
_rpmostree_libhif_repos_enable_by_name (HifContext    *context,
                                        const char    *name,
                                        GError       **error)
{
  gboolean ret = FALSE;
  GPtrArray *sources;
  guint i;
  gboolean found = FALSE;

  sources = hif_context_get_sources (context);
  for (i = 0; i < sources->len; i++)
    {
      HifSource *src = sources->pdata[i];
      const char *id = hif_source_get_id (src);

      if (strcmp (name, id) != 0)
        continue;
      
      hif_source_set_enabled (src, HIF_SOURCE_ENABLED_PACKAGES);
#ifdef HAVE_HIF_SOURCE_SET_REQUIRED
      hif_source_set_required (src, TRUE);
#endif
      found = TRUE;
      break;
    }

  if (!found)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Unknown rpm-md repository: %s", name);
      goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

static void
on_hifstate_percentage_changed (HifState   *hifstate,
                                guint       percentage,
                                gpointer    user_data)
{
  const char *text = user_data;
  glnx_console_progress_text_percent (text, percentage);
}

gboolean
_rpmostree_libhif_console_download_metadata (HifContext     *hifctx,
                                             GCancellable   *cancellable,
                                             GError        **error)
{
  gboolean ret = FALSE;
  guint progress_sigid;
  g_auto(GLnxConsoleRef) console = { 0, };
  gs_unref_object HifState *hifstate = hif_state_new ();

  progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                     G_CALLBACK (on_hifstate_percentage_changed), 
                                     "Downloading metadata:");

  glnx_console_lock (&console);

  if (!hif_context_setup_sack (hifctx, hifstate, error))
    goto out;

  g_signal_handler_disconnect (hifstate, progress_sigid);

  ret = TRUE;
 out:
  _rpmostree_reset_rpm_sighandlers ();
  return ret;
}

static void
append_quoted (GString *r, const char *value)
{
  const char *p;

  for (p = value; *p; p++)
    {
      const char c = *p;
      switch (c)
        {
        case '.':
        case '-':
          g_string_append_c (r, c);
          continue;
        }
      if (g_ascii_isalnum (c))
        {
          g_string_append_c (r, c);
          continue;
        }
      if (c == '_')
        {
          g_string_append (r, "__");
          continue;
        }

      g_string_append_printf (r, "_%02X", c);
    }
}

static char *
cache_branch_for_n_evr_a (const char *name, const char *evr, const char *arch)
{
  GString *r = g_string_new ("rpmostree/pkg/");
  append_quoted (r, name);
  g_string_append_c (r, '/');
  append_quoted (r, evr);
  g_string_append_c (r, '.');
  append_quoted (r, arch);
  return g_string_free (r, FALSE);
}

char *
_rpmostree_get_cache_branch_header (Header hdr)
{
  g_autofree char *name = headerGetAsString (hdr, RPMTAG_NAME);
  g_autofree char *evr = headerGetAsString (hdr, RPMTAG_EVR);
  g_autofree char *arch = headerGetAsString (hdr, RPMTAG_ARCH);
  return cache_branch_for_n_evr_a (name, evr, arch);
}
  
char *
_rpmostree_get_cache_branch_pkg (HyPackage pkg)
{
  return cache_branch_for_n_evr_a (hy_package_get_name (pkg),
                                   hy_package_get_evr (pkg),
                                   hy_package_get_arch (pkg));
}

static gboolean
pkg_is_local (HyPackage pkg)
{
  HifSource *src = hif_package_get_source (pkg);
  return (hif_source_is_local (src) || 
          g_strcmp0 (hy_package_get_reponame (pkg), HY_CMDLINE_REPO_NAME) == 0);
}
  
static gboolean
get_packages_to_download (HifContext  *hifctx,
                          OstreeRepo  *ostreerepo,
                          GPtrArray  **out_packages,
                          GError     **error)
{
  gboolean ret = FALSE;
  guint i;
  g_autoptr(GPtrArray) packages = NULL;
  g_autoptr(GPtrArray) packages_to_download = NULL;
  GPtrArray *sources = hif_context_get_sources (hifctx);
  
  packages = hif_goal_get_packages (hif_context_get_goal (hifctx),
                                    HIF_PACKAGE_INFO_INSTALL,
                                    HIF_PACKAGE_INFO_REINSTALL,
                                    HIF_PACKAGE_INFO_DOWNGRADE,
                                    HIF_PACKAGE_INFO_UPDATE,
                                    -1);
  packages_to_download = g_ptr_array_new_with_free_func ((GDestroyNotify)hy_package_free);
  
  for (i = 0; i < packages->len; i++)
    {
      HyPackage pkg = packages->pdata[i];
      HifSource *src = NULL;
      guint j;

      /* get correct package source */

      /* Hackily look up the source...we need a hash table */
      for (j = 0; j < sources->len; j++)
        {
          HifSource *tmpsrc = sources->pdata[j];
          if (g_strcmp0 (hy_package_get_reponame (pkg),
                         hif_source_get_id (tmpsrc)) == 0)
            {
              src = tmpsrc;
              break;
            }
        }

      g_assert (src);
      hif_package_set_source (pkg, src);

      /* this is a local file */

      if (ostreerepo)
        {
          g_autofree char *cachebranch = _rpmostree_get_cache_branch_pkg (pkg);
          g_autofree char *cached_rev = NULL; 

          if (!ostree_repo_resolve_rev (ostreerepo, cachebranch, TRUE, &cached_rev, error))
            goto out;

          if (cached_rev)
            continue;
        }
      else if (!pkg_is_local (pkg))
        {
          const char *cachepath;
          
          cachepath = hif_package_get_filename (pkg);

          /* Right now we're not re-checksumming cached RPMs, we
           * assume they are valid.  This is a change from the current
           * libhif behavior, but I think it's right.  We should
           * record validity once, then ensure it's immutable after
           * that - which is what happens with the ostree commits
           * above.
           */
          if (g_file_test (cachepath, G_FILE_TEST_EXISTS))
            continue;
        }

      g_ptr_array_add (packages_to_download, hy_package_link (pkg));
    }

  ret = TRUE;
  *out_packages = g_steal_pointer (&packages_to_download);
 out:
  return ret;
}

gboolean
_rpmostree_libhif_console_prepare_install (HifContext           *hifctx,
                                           OstreeRepo           *ostreerepo,
                                           const char *const    *pkgnames,
                                           RpmOstreeHifInstall  *out_install,
                                           GCancellable         *cancellable,
                                           GError              **error)
{
  gboolean ret = FALSE;

  out_install->packages_requested = g_ptr_array_new_with_free_func (g_free);
  { const char *const*strviter = pkgnames;
    for (; strviter && *strviter; strviter++)
      {
        const char *pkgname = *strviter;
        if (!hif_context_install (hifctx, pkgname, error))
          goto out;
        g_ptr_array_add (out_install->packages_requested, g_strdup (pkgname));
      }
  }

  printf ("%s", "Resolving dependencies: ");
  fflush (stdout);

  if (!hif_goal_depsolve (hif_context_get_goal (hifctx), error))
    {
      printf ("%s", "failed\n");
      goto out;
    }
  printf ("%s", "done\n");

  if (!get_packages_to_download (hifctx, ostreerepo, &out_install->packages_to_download, error))
    goto out;

  rpmostree_print_transaction (hifctx);
  g_print ("\n  Need to download %u packages\n", out_install->packages_to_download->len);

  ret = TRUE;
 out:
  return ret;
}

struct GlobalDownloadState {
  struct RpmOstreeHifInstall *install;
  HifState *hifstate;
  gchar *last_mirror_url;
  gchar *last_mirror_failure_message;
};

struct PkgDownloadState {
  struct GlobalDownloadState *gdlstate;
  gboolean added_total;
  guint64 last_bytes_fetched;
  gchar *last_mirror_url;
  gchar *last_mirror_failure_message;
};

static int
package_download_update_state_cb (void *user_data,
				  gdouble total_to_download,
				  gdouble now_downloaded)
{
  struct PkgDownloadState *dlstate = user_data;
  struct RpmOstreeHifInstall *install = dlstate->gdlstate->install;
  if (!dlstate->added_total)
    {
      dlstate->added_total = TRUE;
      install->n_bytes_to_fetch += (guint64) total_to_download;
    }

  install->n_bytes_fetched += ((guint64)now_downloaded) - dlstate->last_bytes_fetched;
  dlstate->last_bytes_fetched = ((guint64)now_downloaded);
  return LR_CB_OK;
}

static int
mirrorlist_failure_cb (void *user_data,
		       const char *message,
		       const char *url)
{
  struct PkgDownloadState *dlstate = user_data;
  struct GlobalDownloadState *gdlstate = dlstate->gdlstate;

  if (gdlstate->last_mirror_url)
    goto out;

  gdlstate->last_mirror_url = g_strdup (url);
  gdlstate->last_mirror_failure_message = g_strdup (message);
 out:
  return LR_CB_OK;
}

static inline void
hif_state_assert_done (HifState *hifstate)
{
  gboolean r;
  r = hif_state_done (hifstate, NULL);
  g_assert (r);
}

static int
package_download_complete_cb (void *user_data,
                              LrTransferStatus status,
                              const char *msg)
{
  struct PkgDownloadState *dlstate = user_data;
  switch (status)
    {
    case LR_TRANSFER_SUCCESSFUL:
    case LR_TRANSFER_ALREADYEXISTS:
      dlstate->gdlstate->install->n_packages_fetched++;
      hif_state_assert_done (dlstate->gdlstate->hifstate);
      return LR_CB_OK;
    case LR_TRANSFER_ERROR:
      return LR_CB_ERROR; 
    default:
      g_assert_not_reached ();
      return LR_CB_ERROR;
    }
}

static int
ptrarray_sort_compare_strings (gconstpointer ap,
                               gconstpointer bp)
{
  char **asp = (gpointer)ap;
  char **bsp = (gpointer)bp;
  return strcmp (*asp, *bsp);
}

/* Generate a checksum from a goal in a repeatable fashion (ordered
 * hash of component NEVRAs).  This can be used to see if the goal has
 * changed from a previous one efficiently.
 */
void
_rpmostree_hif_add_checksum_goal (GChecksum *checksum,
                                  HyGoal     goal)
{
  _cleanup_hypackagelist_ HyPackageList pkglist = NULL;
  HyPackage pkg;
  guint i;
  g_autoptr(GPtrArray) nevras = g_ptr_array_new_with_free_func (g_free);

  pkglist = hy_goal_list_installs (goal);
  FOR_PACKAGELIST(pkg, pkglist, i)
    {
      g_ptr_array_add (nevras, hy_package_get_nevra (pkg));
    }

  g_ptr_array_sort (nevras, ptrarray_sort_compare_strings);
    
  for (i = 0; i < nevras->len; i++)
    {
      const char *nevra = nevras->pdata[i];
      g_checksum_update (checksum, (guint8*)nevra, strlen (nevra));
    }
}

char *
_rpmostree_hif_checksum_goal (GChecksumType ctype, HyGoal goal)
{
  g_autoptr(GChecksum) checksum = g_checksum_new (ctype);
  _rpmostree_hif_add_checksum_goal (checksum, goal);
  return g_strdup (g_checksum_get_string (checksum));
}

/**
 * hif_source_checksum_hy_to_lr:
 **/
static LrChecksumType
hif_source_checksum_hy_to_lr (int checksum_hy)
{
	if (checksum_hy == HY_CHKSUM_MD5)
		return LR_CHECKSUM_MD5;
	if (checksum_hy == HY_CHKSUM_SHA1)
		return LR_CHECKSUM_SHA1;
	if (checksum_hy == HY_CHKSUM_SHA256)
		return LR_CHECKSUM_SHA256;
	return LR_CHECKSUM_UNKNOWN;
}

static gboolean
source_download_packages (HifSource *source,
                          GPtrArray *packages,
                          RpmOstreeHifInstall *install,
                          int        target_dfd,
                          HifState  *state,
                          GCancellable *cancellable,
                          GError **error)
{
  gboolean ret = FALSE;
  char *checksum_str = NULL;
  const unsigned char *checksum;
  guint i;
  int checksum_type;
  LrPackageTarget *target = NULL;
  GSList *package_targets = NULL;
  struct GlobalDownloadState gdlstate = { 0, };
  g_autoptr(GArray) pkg_dlstates = g_array_new (FALSE, TRUE, sizeof (struct PkgDownloadState));
  LrHandle *handle;
  g_autoptr(GError) error_local = NULL;
  g_autofree char *target_dir = NULL;

  handle = hif_source_get_lrhandle (source);

  gdlstate.install = install;
  gdlstate.hifstate = state;

  g_array_set_size (pkg_dlstates, packages->len);
  hif_state_set_number_steps (state, packages->len);

  for (i = 0; i < packages->len; i++)
    {
      g_autofree char *target_dir = NULL;
      HyPackage pkg = packages->pdata[i];
      struct PkgDownloadState *dlstate;

      if (target_dfd == -1)
        {
          target_dir = g_build_filename (hif_source_get_location (source), "/packages/", NULL);
          if (!glnx_shutil_mkdir_p_at (AT_FDCWD, target_dir, 0755, cancellable, error))
            goto out;
        }
      else
        target_dir = glnx_fdrel_abspath (target_dfd, ".");
      
      checksum = hy_package_get_chksum (pkg, &checksum_type);
      checksum_str = hy_chksum_str (checksum, checksum_type);
      
      dlstate = &g_array_index (pkg_dlstates, struct PkgDownloadState, i);
      dlstate->gdlstate = &gdlstate;

      target = lr_packagetarget_new_v2 (handle,
                                        hy_package_get_location (pkg),
                                        target_dir,
                                        hif_source_checksum_hy_to_lr (checksum_type),
                                        checksum_str,
                                        0, /* size unknown */
                                        hy_package_get_baseurl (pkg),
                                        TRUE,
                                        package_download_update_state_cb,
                                        dlstate,
                                        package_download_complete_cb,
                                        mirrorlist_failure_cb,
                                        error);
      if (target == NULL)
        goto out;
	
      package_targets = g_slist_prepend (package_targets, target);
    }

  _rpmostree_reset_rpm_sighandlers ();

  if (!lr_download_packages (package_targets, LR_PACKAGEDOWNLOAD_FAILFAST, &error_local))
    {
      if (g_error_matches (error_local,
                           LR_PACKAGE_DOWNLOADER_ERROR,
                           LRE_ALREADYDOWNLOADED))
        {
          /* ignore */
          g_clear_error (&error_local);
        }
      else
        {
          if (gdlstate.last_mirror_failure_message)
            {
              g_autofree gchar *orig_message = error_local->message;
              error_local->message = g_strconcat (orig_message, "; Last error: ", gdlstate.last_mirror_failure_message, NULL);
            }
          g_propagate_error (error, g_steal_pointer (&error_local));
          goto out;
        }
  } 

  ret = TRUE;
 out:
  g_free (gdlstate.last_mirror_failure_message);
  g_free (gdlstate.last_mirror_url);
  g_slist_free_full (package_targets, (GDestroyNotify)lr_packagetarget_free);
  hy_free (checksum_str);
  return ret;
}

static GHashTable *
gather_source_to_packages (HifContext *hifctx,
                           RpmOstreeHifInstall *install)
{
  guint i;
  g_autoptr(GHashTable) source_to_packages =
    g_hash_table_new_full (NULL, NULL, NULL, (GDestroyNotify)g_ptr_array_unref);

  for (i = 0; i < install->packages_to_download->len; i++)
    {
      HyPackage pkg = install->packages_to_download->pdata[i];
      HifSource *src = hif_package_get_source (pkg);
      GPtrArray *source_packages;
      
      g_assert (src);
                     
      source_packages = g_hash_table_lookup (source_to_packages, src);
      if (!source_packages)
        {
          source_packages = g_ptr_array_new ();
          g_hash_table_insert (source_to_packages, src, source_packages);
        }
      g_ptr_array_add (source_packages, pkg);
    }

  return g_steal_pointer (&source_to_packages);
}

gboolean
_rpmostree_libhif_console_download_rpms (HifContext     *hifctx,
                                         int             target_dfd,
                                         RpmOstreeHifInstall *install,
                                         GCancellable   *cancellable,
                                         GError        **error)
{
  gboolean ret = FALSE;
  g_auto(GLnxConsoleRef) console = { 0, };
  guint progress_sigid;
  GHashTableIter hiter;
  gpointer key, value;

  glnx_console_lock (&console);

  { g_autoptr(GHashTable) source_to_packages = gather_source_to_packages (hifctx, install);

    g_hash_table_iter_init (&hiter, source_to_packages);
    while (g_hash_table_iter_next (&hiter, &key, &value))
      {
        HifSource *src = key;
        GPtrArray *src_packages = value;
        gs_unref_object HifState *hifstate = hif_state_new ();
        g_autofree char *prefix = g_strconcat ("Downloading packages (", hif_source_get_id (src), ")", NULL); 

        progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                           G_CALLBACK (on_hifstate_percentage_changed), 
                                           prefix);
      
        if (!source_download_packages (src, src_packages, install, target_dfd, hifstate,
                                       cancellable, error))
          goto out;

        g_signal_handler_disconnect (hifstate, progress_sigid);
      }
  }


  ret = TRUE;
 out:
  return ret;
}

static gboolean
import_one_package (OstreeRepo   *ostreerepo,
                    int           tmpdir_dfd,
                    HifContext   *hifctx,
                    HyPackage     pkg,
                    GCancellable *cancellable,
                    GError      **error)
{
  gboolean ret = FALSE;
  g_autofree char *ostree_commit = NULL;
  glnx_unref_object RpmOstreeUnpacker *unpacker = NULL;
  const char *pkg_relpath;

  if (pkg_is_local (pkg))
    {
      tmpdir_dfd = AT_FDCWD;
      pkg_relpath = hif_package_get_filename (pkg);
    }
  else
    pkg_relpath = glnx_basename (hy_package_get_location (pkg)); 
   
  /* TODO - tweak the unpacker flags for containers */
  unpacker = rpmostree_unpacker_new_at (tmpdir_dfd, pkg_relpath,
                                        RPMOSTREE_UNPACKER_FLAGS_ALL,
                                        error);
  if (!unpacker)
    goto out;

  if (!rpmostree_unpacker_unpack_to_ostree (unpacker, ostreerepo, NULL, &ostree_commit,
                                            cancellable, error))
    {
      g_autofree char *nevra = hy_package_get_nevra (pkg);
      g_prefix_error (error, "Unpacking %s: ", nevra);
      goto out;
    }
   
  if (!pkg_is_local (pkg))
    {
      if (TEMP_FAILURE_RETRY (unlinkat (tmpdir_dfd, pkg_relpath, 0)) < 0)
        {
          glnx_set_error_from_errno (error);
          g_prefix_error (error, "Deleting %s: ", pkg_relpath);
          goto out;
        }
    }

  ret = TRUE;
 out:
  return ret;
}

gboolean
_rpmostree_libhif_console_download_import (HifContext           *hifctx,
                                           OstreeRepo           *ostreerepo,
                                           RpmOstreeHifInstall  *install,
                                           GCancellable         *cancellable,
                                           GError              **error)
{
  gboolean ret = FALSE;
  g_auto(GLnxConsoleRef) console = { 0, };
  glnx_fd_close int pkg_tempdir_dfd = -1;
  g_autofree char *pkg_tempdir = NULL;
  guint progress_sigid;
  GHashTableIter hiter;
  gpointer key, value;
  guint i;

  glnx_console_lock (&console);

  if (!rpmostree_mkdtemp ("/var/tmp/rpmostree-import-XXXXXX", &pkg_tempdir, &pkg_tempdir_dfd, error))
    goto out;

  { g_autoptr(GHashTable) source_to_packages = gather_source_to_packages (hifctx, install);
    
    g_hash_table_iter_init (&hiter, source_to_packages);
    while (g_hash_table_iter_next (&hiter, &key, &value))
      {
        glnx_unref_object HifState *hifstate = hif_state_new ();
        HifSource *src = key;
        GPtrArray *src_packages = value;
        g_autofree char *prefix = g_strconcat ("Downloading from ", hif_source_get_id (src), NULL); 

        if (hif_source_is_local (src))
          continue;

        progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                           G_CALLBACK (on_hifstate_percentage_changed), 
                                           prefix);
        
        if (!source_download_packages (src, src_packages, install, pkg_tempdir_dfd, hifstate,
                                       cancellable, error))
          goto out;

        g_signal_handler_disconnect (hifstate, progress_sigid);
      }
  }

  { 
    glnx_unref_object HifState *hifstate = hif_state_new ();

    hif_state_set_number_steps (hifstate, install->packages_to_download->len);
    progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                       G_CALLBACK (on_hifstate_percentage_changed), 
                                       "Importing packages:");

    for (i = 0; i < install->packages_to_download->len; i++)
      {
        HyPackage pkg = install->packages_to_download->pdata[i];
        if (!import_one_package (ostreerepo, pkg_tempdir_dfd, hifctx, pkg,
                                 cancellable, error))
          goto out;
        hif_state_assert_done (hifstate);
      }

    g_signal_handler_disconnect (hifstate, progress_sigid);
  }


  ret = TRUE;
 out:
  return ret;
}

static gboolean
ostree_checkout_package (int           dfd,
                         const char   *path,
                         HifContext   *hifctx,
                         HyPackage     pkg,
                         OstreeRepo   *ostreerepo,
                         OstreeRepoDevInoCache *devino_cache,
                         const char   *pkg_ostree_commit,
                         GCancellable *cancellable,
                         GError      **error)
{
  gboolean ret = FALSE;
  OstreeRepoCheckoutOptions opts = { OSTREE_REPO_CHECKOUT_MODE_USER,
                                     OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES, };

  opts.devino_to_csum_cache = devino_cache;

  /* For now... to be crash safe we'd need to duplicate some of the
   * boot-uuid/fsync gating at a higher level.
   */
  opts.disable_fsync = TRUE;

  if (!ostree_repo_checkout_tree_at (ostreerepo, &opts, dfd, path,
                                     pkg_ostree_commit, cancellable, error))
    goto out;
  
  ret = TRUE;
 out:
  if (error && *error)
    g_prefix_error (error, "Unpacking %s: ", hif_package_get_nevra (pkg));
  return ret;
}

typedef struct {
  FD_t current_trans_fd;
  int tmp_metadata_dfd;
} TransactionData;

static void *
ts_callback (const void * h, 
             const rpmCallbackType what, 
             const rpm_loff_t amount, 
             const rpm_loff_t total,
             fnpyKey key,
             rpmCallbackData data)
{
  TransactionData *tdata = data;

  switch (what)
    {
    case RPMCALLBACK_INST_OPEN_FILE:
      {
        const char *nevra = (char *) key;
        g_autofree char *path = glnx_fdrel_abspath (tdata->tmp_metadata_dfd, nevra);
        g_assert (tdata->current_trans_fd == NULL);
        tdata->current_trans_fd = Fopen (path, "r.ufdio");
        return tdata->current_trans_fd;
      }
      break;

    case RPMCALLBACK_INST_CLOSE_FILE:
      g_clear_pointer (&tdata->current_trans_fd, Fclose);
      break;

    default:
      break;
    }

  return NULL;
}

static gboolean
add_to_transaction (rpmts  ts,
                    HyPackage pkg,
                    int tmp_metadata_dfd,
                    GError **error)
{
  gboolean ret = FALSE;
  int r;
  Header hdr = NULL;
  glnx_fd_close int metadata_fd = -1;

  if ((metadata_fd = openat (tmp_metadata_dfd, hif_package_get_nevra (pkg), O_RDONLY | O_CLOEXEC)) < 0)
    {
      glnx_set_error_from_errno (error);
      goto out;
    }

  if (!rpmostree_unpacker_read_metainfo (metadata_fd, &hdr, NULL, NULL, error))
    goto out;
          
  r = rpmtsAddInstallElement (ts, hdr, (char*)hif_package_get_nevra (pkg), TRUE, NULL);
  if (r != 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Failed to add install element for %s",
                   hif_package_get_filename (pkg));
      goto out;
    }

  ret = TRUE;
 out:
  if (hdr)
    headerFree (hdr);
  return ret;
}

static void
set_rpm_macro_define (const char *key, const char *value)
{
  g_autofree char *buf = g_strconcat ("%define ", key, " ", value, NULL);
  /* Calling expand with %define (ignoring the return
   * value) is apparently the way to change the global
   * macro context.
   */
  free (rpmExpand (buf, NULL));
}

gboolean
_rpmostree_libhif_console_assemble_commit (HifContext    *hifctx,
                                           int            tmpdir_dfd,
                                           OstreeRepo    *ostreerepo,
                                           const char    *name,
                                           struct RpmOstreeHifInstall *install,
                                           char          **out_commit,
                                           GCancellable  *cancellable,
                                           GError       **error)
{
  gboolean ret = FALSE;
  guint progress_sigid;
  TransactionData tdata = { 0, -1 };
  g_autofree char *root_abspath = NULL;
  g_auto(GLnxConsoleRef) console = { 0, };
  glnx_unref_object HifState *hifstate = NULL;
  rpmts ordering_ts = NULL;
  rpmts rpmdb_ts = NULL;
  guint i, n_rpmts_elements;
  g_autoptr(GHashTable) nevra_to_pkg =
    g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify)hy_package_free);
  g_autoptr(GHashTable) pkg_to_ostree_commit =
    g_hash_table_new_full (NULL, NULL, (GDestroyNotify)hy_package_free, (GDestroyNotify)g_free);
  HyPackage filesystem_package = NULL;   /* It's special... */
  int r;
  glnx_fd_close int rootfs_fd = -1;
  char *tmp_metadata_dir_path = NULL;
  glnx_fd_close int tmp_metadata_dfd = -1;
  OstreeRepoDevInoCache *devino_cache = NULL;
  g_autofree char *workdir_path = NULL;
  g_autofree char *ret_commit_checksum = NULL;
  const char *workdir_root;

  glnx_console_lock (&console);

  if (!rpmostree_mkdtemp ("/tmp/rpmostree-metadata-XXXXXX", &tmp_metadata_dir_path,
                          &tmp_metadata_dfd, error))
    goto out;
  tdata.tmp_metadata_dfd = tmp_metadata_dfd;

  workdir_path = g_strdup ("rpmostree-commit-XXXXXX");
  if (!glnx_mkdtempat (tmpdir_dfd, workdir_path, 0755, error))
    goto out;

  workdir_root = glnx_strjoina (workdir_path, "/root");
  root_abspath = glnx_fdrel_abspath (tmpdir_dfd, workdir_root);

  ordering_ts = rpmtsCreate ();
  rpmtsSetRootDir (ordering_ts, root_abspath);
  /* First for the ordering TS, set the dbpath to relative, which will also gain
   * the root dir.
   */
  set_rpm_macro_define ("_dbpath", "/usr/share/rpm");

  /* Don't verify checksums here (we should have done this on ostree
   * import).  Also, when we do run the transaction, only update the
   * rpmdb.  Otherwise we unpacked with cpio.
   */
  rpmtsSetVSFlags (ordering_ts, _RPMVSF_NOSIGNATURES | _RPMVSF_NODIGESTS | RPMTRANS_FLAG_JUSTDB);

  /* Tell librpm about each one so it can tsort them.  What we really
   * want is to do this from the rpm-md metadata so that we can fully
   * parallelize download + unpack.
   */
  { g_autoptr(GPtrArray) package_list = NULL;

    package_list = hif_goal_get_packages (hif_context_get_goal (hifctx),
                                          HIF_PACKAGE_INFO_INSTALL,
                                          HIF_PACKAGE_INFO_REINSTALL,
                                          HIF_PACKAGE_INFO_DOWNGRADE,
                                          HIF_PACKAGE_INFO_UPDATE,
                                          -1);

    for (i = 0; i < package_list->len; i++)
      {
        HyPackage pkg = package_list->pdata[i];
        glnx_unref_object RpmOstreeUnpacker *unpacker = NULL;
        g_autofree char *cachebranch = _rpmostree_get_cache_branch_pkg (pkg);
        g_autofree char *cached_rev = NULL; 
        g_autoptr(GVariant) pkg_commit = NULL;
        g_autoptr(GVariant) header_variant = NULL;

        if (!ostree_repo_resolve_rev (ostreerepo, cachebranch, FALSE, &cached_rev, error))
          goto out;

        if (!ostree_repo_load_variant (ostreerepo, OSTREE_OBJECT_TYPE_COMMIT, cached_rev,
                                       &pkg_commit, error))
          goto out;

        { g_autoptr(GVariant) pkg_meta = g_variant_get_child_value (pkg_commit, 0);
          g_autoptr(GVariantDict) pkg_meta_dict = g_variant_dict_new (pkg_meta);

          header_variant = _rpmostree_vardict_lookup_value_required (pkg_meta_dict, "rpmostree.metadata",
                                                                     (GVariantType*)"ay", error);
          if (!header_variant)
            {
              g_prefix_error (error, "In commit %s of %s: ", cached_rev, hif_package_get_id (pkg));
              goto out;
            }

          if (!glnx_file_replace_contents_at (tmp_metadata_dfd, hif_package_get_nevra (pkg),
                                              g_variant_get_data (header_variant),
                                              g_variant_get_size (header_variant),
                                              GLNX_FILE_REPLACE_NODATASYNC,
                                              cancellable, error))
            goto out;

          if (!add_to_transaction (ordering_ts, pkg, tmp_metadata_dfd, error))
            goto out;
        }

        g_hash_table_insert (nevra_to_pkg, (char*)hif_package_get_nevra (pkg), hy_package_link (pkg));
        g_hash_table_insert (pkg_to_ostree_commit, hy_package_link (pkg), g_steal_pointer (&cached_rev));

        if (strcmp (hy_package_get_name (pkg), "filesystem") == 0)
          filesystem_package = hy_package_link (pkg);
      }
  }

  rpmtsOrder (ordering_ts);
  _rpmostree_reset_rpm_sighandlers ();

  hifstate = hif_state_new ();
  progress_sigid = g_signal_connect (hifstate, "percentage-changed",
                                     G_CALLBACK (on_hifstate_percentage_changed), 
                                     "Unpacking:");


  n_rpmts_elements = (guint)rpmtsNElements (ordering_ts);
  hif_state_set_number_steps (hifstate, n_rpmts_elements);

  devino_cache = ostree_repo_devino_cache_new ();

  /* Okay so what's going on in Fedora with incestuous relationship
   * between the `filesystem`, `setup`, `libgcc` RPMs is actively
   * ridiculous.  If we unpack libgcc first it writes to /lib64 which
   * is really /usr/lib64, then filesystem blows up since it wants to symlink
   * /lib64 -> /usr/lib64.
   *
   * Really `filesystem` should be first but it depends on `setup` for
   * stupid reasons which is hacked around in `%pretrans` which we
   * don't run.  Just forcibly unpack it first.
   */
  if (filesystem_package)
    {
      if (!ostree_checkout_package (tmpdir_dfd, workdir_path, hifctx, filesystem_package, ostreerepo, devino_cache,
                                    g_hash_table_lookup (pkg_to_ostree_commit, filesystem_package),
                                    cancellable, error))
        goto out;
      hif_state_assert_done (hifstate);
    }
  else
    {
      /* Otherwise, we unpack the first package to get the initial
       * rootfs dir.
       */
      rpmte te = rpmtsElement (ordering_ts, 0);
      const char *tekey = rpmteKey (te);
      HyPackage pkg = g_hash_table_lookup (nevra_to_pkg, tekey);

      if (!ostree_checkout_package (tmpdir_dfd, workdir_path, hifctx, pkg, ostreerepo, devino_cache,
                                    g_hash_table_lookup (pkg_to_ostree_commit, pkg),
                                    cancellable, error))
        goto out;
      hif_state_assert_done (hifstate);
    }

  if (!glnx_opendirat (tmpdir_dfd, workdir_path, FALSE, &rootfs_fd, error))
    goto out;

  if (filesystem_package)
    i = 0;
  else
    i = 1;

  for (; i < n_rpmts_elements; i++)
    {
      rpmte te = rpmtsElement (ordering_ts, i);
      const char *tekey = rpmteKey (te);
      HyPackage pkg = g_hash_table_lookup (nevra_to_pkg, tekey);

      if (pkg == filesystem_package)
        continue;

      if (!ostree_checkout_package (rootfs_fd, ".", hifctx, pkg, ostreerepo, devino_cache,
                                    g_hash_table_lookup (pkg_to_ostree_commit, pkg),
                                    cancellable, error))
        goto out;
      hif_state_assert_done (hifstate);
    }

  glnx_console_unlock (&console);

  g_print ("Writing rpmdb...\n");

  if (!glnx_shutil_mkdir_p_at (rootfs_fd, "usr/share/rpm", 0755, cancellable, error))
    goto out;

  /* Now, we use the separate rpmdb ts which *doesn't* have a rootdir
   * set, because if it did rpmtsRun() would try to chroot which it
   * can't, even though we're not trying to run %post scripts now.
   *
   * Instead, this rpmts has the dbpath as absolute.
   */
  { g_autofree char *rpmdb_abspath = glnx_fdrel_abspath (rootfs_fd, "usr/share/rpm");
    set_rpm_macro_define ("_dbpath", rpmdb_abspath);
  }

  rpmdb_ts = rpmtsCreate ();
  rpmtsSetVSFlags (rpmdb_ts, _RPMVSF_NOSIGNATURES | _RPMVSF_NODIGESTS);
  rpmtsSetFlags (rpmdb_ts, RPMTRANS_FLAG_JUSTDB);
  rpmtsSetNotifyCallback (rpmdb_ts, ts_callback, &tdata);

  { gpointer k,v;
    GHashTableIter hiter;

    g_hash_table_iter_init (&hiter, pkg_to_ostree_commit);
    while (g_hash_table_iter_next (&hiter, &k, &v))
      {
        HyPackage pkg = k;

        if (!add_to_transaction (rpmdb_ts, pkg, tmp_metadata_dfd, error))
          goto out;
      }
  }

  rpmtsOrder (rpmdb_ts);
  r = rpmtsRun (rpmdb_ts, NULL, 0);
  if (r < 0)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "Failed to update rpmdb (rpmtsRun code %d)", r);
      goto out;
    }

  g_print ("Writing rpmdb...done\n");

  g_signal_handler_disconnect (hifstate, progress_sigid);

  g_print ("Writing OSTree commit...\n");

  if (!ostree_repo_prepare_transaction (ostreerepo, NULL, cancellable, error))
    goto out;

  { glnx_unref_object OstreeMutableTree *mtree = NULL;
    g_autoptr(GFile) root = NULL;
    g_auto(GVariantBuilder) metadata_builder;
    g_autofree char *goal_checksum = NULL;
    OstreeRepoCommitModifier *commit_modifier = ostree_repo_commit_modifier_new (OSTREE_REPO_COMMIT_MODIFIER_FLAGS_NONE, NULL, NULL, NULL);

    g_variant_builder_init (&metadata_builder, (GVariantType*)"a{sv}");

    g_variant_builder_add (&metadata_builder, "{sv}",
                           "rpmostree.input-packages", g_variant_new_strv ((const char *const*)install->packages_requested->pdata,
                                                                           install->packages_requested->len));

    goal_checksum = _rpmostree_hif_checksum_goal (G_CHECKSUM_SHA512, hif_context_get_goal (hifctx));
    g_variant_builder_add (&metadata_builder, "{sv}",
                           "rpmostree.nevras-sha512", g_variant_new_string (goal_checksum));

    ostree_repo_commit_modifier_set_devino_cache (commit_modifier, devino_cache);

    mtree = ostree_mutable_tree_new ();

    if (!ostree_repo_write_dfd_to_mtree (ostreerepo, rootfs_fd, ".", mtree,
                                         commit_modifier, cancellable, error))
      goto out;

    if (!ostree_repo_write_mtree (ostreerepo, mtree, &root, cancellable, error))
      goto out;
    
    if (!ostree_repo_write_commit (ostreerepo, NULL, "", "",
                                   g_variant_builder_end (&metadata_builder),
                                   OSTREE_REPO_FILE (root),
                                   &ret_commit_checksum, cancellable, error))
      goto out;
    
    ostree_repo_transaction_set_ref (ostreerepo, NULL, name, ret_commit_checksum);

    if (!ostree_repo_commit_transaction (ostreerepo, NULL, cancellable, error))
      goto out;

    /* FIXME memleak on error, need g_autoptr for this */
    ostree_repo_commit_modifier_unref (commit_modifier);
  }

  g_print ("Writing OSTree commit...done\n");

  ret = TRUE;
  if (out_commit)
    *out_commit = g_steal_pointer (&ret_commit_checksum);
 out:
  if (devino_cache)
    ostree_repo_devino_cache_unref (devino_cache);
  if (ordering_ts)
    rpmtsFree (ordering_ts);
  if (rpmdb_ts)
    rpmtsFree (rpmdb_ts);
  if (tmp_metadata_dir_path)
    (void) glnx_shutil_rm_rf_at (AT_FDCWD, tmp_metadata_dir_path, cancellable, NULL);
  if (workdir_path)
    (void) glnx_shutil_rm_rf_at (tmpdir_dfd, workdir_path, cancellable, NULL);
  return ret;
}