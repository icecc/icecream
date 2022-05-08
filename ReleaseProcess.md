This is an early draft of the release process for icecc.  It will need to be refined over time.

An outline of the process is:

1. Verify the NEWS file is up to date
  1. This should be done when each feature is merged, but rarely is.
1. Create a branch for the new release
  1. update the version in master
1. Create a release candidate (optional)
1. Test
1. Run `make distcheck` and fix errors
1. Create a full release
  1. Update the version numbers in the release branch
    1. Push this change
  1. Run `make dist`
    1. Verify creates packages build in a clean directory
  1. On GitHub create a new release
    1. Upload the files create by `make dist`
    1. Create a new tag for this release
    1. Add any notes that are particularly interesting
1. Repeat the release process if needed for minor releases from this branch, or start over with a new release from master

# Updating versions Numbers
Version numbers need to be updated several times in this process. 

Master need to always have the version numbers like 1.X.90 where X is the version from the previous release.
Official releases have versions numbers of 1.X.Y where X is the new release number (minor version) and Y increases as for each "micro" release.  Typically icecc does not do micro releases, but sometimes a release is done wrong and so a correction release is made, or an important bug is fixed.

In the file `configure.ac` find the lines for icecream_version_minor and icecream_version_micro, update these as needed.  The following example is for version 1.4.90, and so is what you might expect to see in the master branch until release 1.5.0 is made.
```
m4_define([icecream_version_minor],[4])
m4_define([icecream_version_micro],[90])
```
Commit the above changes and push to the correct branch.
  
# Create the release branch

We name our branches `1.X-branch` where X matches the version minor or the release.

Branches can be created on GitHub, or on your machine and pushed.

# Create a release candidate
Release candidates get a version version minor matching the previous release, and a version micro of 91 (or more if a additional release candidates are needed).  They are tagged as 1.Xrc1 where X is the expected future version minor. rc1 would be updated if a second release candidate is needed.

Other than the above we follow the same process as for a full release.

# make distcheck

Run the `make distcheck` target to verify the distribution packages work.  This should be self explanatory, but if there are errors you may need to fix them.

# Doing a release

Update your local repository to the correct release branch. Make sure your repository is clean of all other files, often done by running `make distclean`

Update the version numbers (see Updating versions Numbers above)

## make dist

You should at this point be at a completely pristine repository, which needs to be prepared as normal.

```sh
./autogen.sh
./configure
make dist
```
This will create two files: icecc-1.4.0.tar.gz and icecc-1.4.0.tar.xz, the filenames should reflect the exact version you think you are releasing

### test dist packages
Create a test directory someplace else on your system.  Extract one of the above files to that directory and verify it builds

```sh
cd
mkdir icecc-test
cd icecc-test
tar -zxf ../path/to/icecc/src/icecc-1.4.0.tar.gz
./configure
make test
```

If possible repeat the above on any different OS you have access to. 

## Create GitHub release

On GitHub.com find the "draft a new release button" and press it.  (this is likely to move and/or be renamed by GitHub).  Feel free to search for help on how to run the release process, it may be better documented elsewhere.

Set the branch to your release branch.  Create a tag for this release- should be 1.X where X is the minor version of this release.  (or 1.X.Y if this is a micro release)

Name the release - generally "1.X Release"

Add any other release comments you feel are useful.

Upload the two files created by the `make dist` command above.

Double check that everything looks right, then publish the release.


