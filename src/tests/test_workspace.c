#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aimee.h"
#include "workspace.h"

static void remove_tree(const char *path)
{
   DIR *dir = opendir(path);
   if (!dir)
      return;

   struct dirent *ent;
   while ((ent = readdir(dir)) != NULL)
   {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
         continue;

      char child[1024];
      struct stat st;
      snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
      if (stat(child, &st) == 0 && S_ISDIR(st.st_mode))
         remove_tree(child);
      else
         unlink(child);
   }
   closedir(dir);
   rmdir(path);
}

static void create_git_repo(const char *path)
{
   mkdir(path, 0755);
   char git_dir[1024];
   snprintf(git_dir, sizeof(git_dir), "%s/.git", path);
   mkdir(git_dir, 0755);
}

int main(void)
{
   printf("workspace: ");

   /* Use isolated temp dir */
   char tmpdir[] = "/tmp/aimee-test-workspace-XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   /* --- discover_projects: empty directory --- */
   {
      char empty[512];
      snprintf(empty, sizeof(empty), "%s/empty", tmpdir);
      mkdir(empty, 0755);

      char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
      int count = workspace_discover_projects(empty, MAX_WORKSPACE_DEPTH, projects,
                                              MAX_DISCOVERED_PROJECTS);
      assert(count == 0);
   }

   /* --- discover_projects: single git repo at root --- */
   {
      char repo[512];
      snprintf(repo, sizeof(repo), "%s/single-repo", tmpdir);
      create_git_repo(repo);

      char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
      int count =
          workspace_discover_projects(repo, MAX_WORKSPACE_DEPTH, projects, MAX_DISCOVERED_PROJECTS);
      assert(count == 1);
      assert(strstr(projects[0], "single-repo") != NULL);
   }

   /* --- discover_projects: multiple repos in subdirectories --- */
   {
      char ws[512];
      snprintf(ws, sizeof(ws), "%s/multi", tmpdir);
      mkdir(ws, 0755);

      char repo1[512], repo2[512], repo3[512];
      snprintf(repo1, sizeof(repo1), "%s/proj-a", ws);
      snprintf(repo2, sizeof(repo2), "%s/proj-b", ws);
      snprintf(repo3, sizeof(repo3), "%s/proj-c", ws);
      create_git_repo(repo1);
      create_git_repo(repo2);
      create_git_repo(repo3);

      /* Also create a non-git directory that should be skipped */
      char non_git[512];
      snprintf(non_git, sizeof(non_git), "%s/docs", ws);
      mkdir(non_git, 0755);

      char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
      int count =
          workspace_discover_projects(ws, MAX_WORKSPACE_DEPTH, projects, MAX_DISCOVERED_PROJECTS);
      assert(count == 3);
   }

   /* --- discover_projects: nested repos (deep directory structure) --- */
   {
      char ws[512];
      snprintf(ws, sizeof(ws), "%s/nested", tmpdir);
      mkdir(ws, 0755);

      char level1[512], level2[512], deep_repo[512];
      snprintf(level1, sizeof(level1), "%s/org", ws);
      mkdir(level1, 0755);
      snprintf(level2, sizeof(level2), "%s/team", level1);
      mkdir(level2, 0755);
      snprintf(deep_repo, sizeof(deep_repo), "%s/deep-proj", level2);
      create_git_repo(deep_repo);

      /* Also a repo at level 1 */
      char shallow_repo[512];
      snprintf(shallow_repo, sizeof(shallow_repo), "%s/shallow-proj", ws);
      create_git_repo(shallow_repo);

      char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
      int count =
          workspace_discover_projects(ws, MAX_WORKSPACE_DEPTH, projects, MAX_DISCOVERED_PROJECTS);
      assert(count == 2);

      /* Verify both repos found */
      int found_deep = 0, found_shallow = 0;
      for (int i = 0; i < count; i++)
      {
         if (strstr(projects[i], "deep-proj"))
            found_deep = 1;
         if (strstr(projects[i], "shallow-proj"))
            found_shallow = 1;
      }
      assert(found_deep);
      assert(found_shallow);
   }

   /* --- discover_projects: does not recurse into git repos --- */
   {
      char ws[512];
      snprintf(ws, sizeof(ws), "%s/no-recurse", tmpdir);
      mkdir(ws, 0755);

      char repo[512];
      snprintf(repo, sizeof(repo), "%s/parent-repo", ws);
      create_git_repo(repo);

      /* Create a nested "git repo" inside the parent repo */
      char nested[512];
      snprintf(nested, sizeof(nested), "%s/submodule", repo);
      create_git_repo(nested);

      char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
      int count =
          workspace_discover_projects(ws, MAX_WORKSPACE_DEPTH, projects, MAX_DISCOVERED_PROJECTS);
      /* Should only find parent-repo, not the nested submodule */
      assert(count == 1);
      assert(strstr(projects[0], "parent-repo") != NULL);
   }

   /* --- discover_projects: depth limiting --- */
   {
      char ws[512];
      snprintf(ws, sizeof(ws), "%s/depth-test", tmpdir);
      mkdir(ws, 0755);

      /* Create a repo 3 levels deep */
      char l1[512], l2[512], l3[512];
      snprintf(l1, sizeof(l1), "%s/a", ws);
      mkdir(l1, 0755);
      snprintf(l2, sizeof(l2), "%s/b", l1);
      mkdir(l2, 0755);
      snprintf(l3, sizeof(l3), "%s/deep-repo", l2);
      create_git_repo(l3);

      /* With depth 1, should NOT find it */
      char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
      int count = workspace_discover_projects(ws, 1, projects, MAX_DISCOVERED_PROJECTS);
      assert(count == 0);

      /* With depth 3, SHOULD find it */
      count = workspace_discover_projects(ws, 3, projects, MAX_DISCOVERED_PROJECTS);
      assert(count == 1);
   }

   /* --- discover_projects: skips noise directories --- */
   {
      char ws[512];
      snprintf(ws, sizeof(ws), "%s/skip-noise", tmpdir);
      mkdir(ws, 0755);

      /* Create node_modules with a .git inside (should be skipped) */
      char nm[512];
      snprintf(nm, sizeof(nm), "%s/node_modules", ws);
      mkdir(nm, 0755);
      char nm_repo[512];
      snprintf(nm_repo, sizeof(nm_repo), "%s/some-pkg", nm);
      create_git_repo(nm_repo);

      /* Create a real project */
      char real[512];
      snprintf(real, sizeof(real), "%s/real-proj", ws);
      create_git_repo(real);

      char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
      int count =
          workspace_discover_projects(ws, MAX_WORKSPACE_DEPTH, projects, MAX_DISCOVERED_PROJECTS);
      assert(count == 1);
      assert(strstr(projects[0], "real-proj") != NULL);
   }

   /* --- style_read: missing file returns NULL --- */
   {
      char *style = style_read("nonexistent-project-xyz");
      assert(style == NULL);
   }

   /* Cleanup */
   remove_tree(tmpdir);

   printf("all tests passed\n");
   return 0;
}
