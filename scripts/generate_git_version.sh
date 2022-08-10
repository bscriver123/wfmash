INC_DIR=$1

GIT_VERSION=$(git describe --always --tags)

echo "#define WFMASH_GIT_VERSION" \"$GIT_VERSION\" > $1/wfmash_git_version.hpp.tmp
diff $1/wfmash_git_version.hpp.tmp $1/wfmash_git_version.hpp >/dev/null || cp $1/wfmash_git_version.hpp.tmp $1/wfmash_git_version.hpp
rm -f $1/wfmash_git_version.hpp.tmp
