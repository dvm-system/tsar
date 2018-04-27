#!/bin/sh
#===--- rbuild.sh -------------- Remote Builder ----------------*- Bash -*-===#
#
#                        Traits Static Analyzer (SAPFOR)
#
#===------------------------------------------------------------------------===#
#
# This script aims to enable remote building of TSAR according to preliminary
# configured remote project.
#
# This requires the following tools:
# - git (on local and remote system to transfer data)
# - ssh (to connect to a remote system)
# - make (on remote system to build project)
#
# The following scenario is supported:
# 1. Commit all changes in current branches of IDB and TSAR repositories
#    to a remote repository with name $REMOTE_REPO_NAME in a force way
#    (git add -all, git commit, git push -f).
# 2. Use ssh to establish connection with a remote system:
#    ssh $REMOTE_HOST -p$REMOTE_PORT
# 3. Stash all changes on a remote local copy of IDB and TSAR repositories.
# 4. Reapply commits on top of remote repository (git rebase) with name
#    $REMOTE_REPO_NAME.
# 5. Enable necessary compiler (source $REMOTE_COMPILER_PATH).
# 6. Use make and specified number of jobs ($BUILD_JOB_NUM) to build project.
# 7. Install analyzer at a specified path $REMOTE_INSTALL_PATH.
# 8. Reset the index and working tree and discard all obtained changes in the
#    working tree of IDB and TSAR repositories (git reset --hard).
# 9. Disconnect from remote system.
#
# The following preliminary configurations should be manually made:
# 1. Remote project should be configured (use CMake).
# 2. Remote repository should be created to transfer data between local
#    and remote systems.
#
# Note, that it is convenient to use SSH key to log in to a remote system
# instead of passwords. It is also convenient to use a separate bare repository
# created on a remote system to perform data transfer without additional
# authentication.
#===------------------------------------------------------------------------===#

# The following variable depends on a remote system and should be set manually.
REMOTE_HOST=""
REMOTE_PORT=22
SAPFOR_REMOTE_REPO=""
REMOTE_REPO_NAME="build"
REMOTE_BUILD_PATH=""
REMOTE_INSTALL_PATH=""
BUILD_JOB_NUM=1
REMOTE_COMPILER_PATH=""
#===------------------------------------------------------------------------===#

TSAR_REMOTE_REPO="$SAPFOR_REMOTE_REPO/analyzers/tsar/trunk"
IDB_REMOTE_REPO="$SAPFOR_REMOTE_REPO/idb/trunk"

SCRIPT_PATH="`dirname \"$0\"`"
SCRIPT_PATH="`(cd \"$SCRIPT_PATH\" && pwd )`"

SAPFOR_PATH="$SCRIPT_PATH\..\..\..\.."
TSAR_PATH="$SAPFOR_PATH\analyzers\tsar\trunk"
IDB_PATH="$SAPFOR_PATH\idb\trunk"

SAPFOR_PATH="`(cd \"$SAPFOR_PATH\" && pwd )`"
TSAR_PATH="`(cd \"$TSAR_PATH\" && pwd )`"
IDB_PATH="`(cd \"$IDB_PATH\" && pwd )`"

# Commit all changes in IDB working tree.
cd $IDB_PATH
CURRENT_IDB_BRANCH="`git rev-parse --abbrev-ref HEAD`"
echo "This is stab to perform commit if there is no other changes." > $REMOTE_REPO_NAME.stab
git add --all
git commit -m "Prepare to build."
git push $REMOTE_REPO_NAME $CURRENT_IDB_BRANCH -f
git reset $REMOTE_REPO_NAME/$CURRENT_IDB_BRANCH^
rm $REMOTE_REPO_NAME.stab

# Commit all changes in TSAR working tree.
cd $TSAR_PATH
CURRENT_TSAR_BRANCH="`git rev-parse --abbrev-ref HEAD`"
echo "This is stab to perform commit if there is no other changes." > $REMOTE_REPO_NAME.stab
git add --all
git commit -m "Prepare to build."
git push $REMOTE_REPO_NAME $CURRENT_TSAR_BRANCH -f
git reset $REMOTE_REPO_NAME/$CURRENT_TSAR_BRANCH^
rm $REMOTE_REPO_NAME.stab

# Connect and build.
ssh $REMOTE_HOST -p$REMOTE_PORT -t "cd $IDB_REMOTE_REPO; git fetch $REMOTE_REPO_NAME; git rebase $REMOTE_REPO_NAME/$CURRENT_IDB_BRANCH; cd $TSAR_REMOTE_REPO; git fetch $REMOTE_REPO_NAME; git stash; git checkout $CURRENT_TSAR_BRANCH; git rebase $REMOTE_REPO_NAME/$CURRENT_TSAR_BRANCH; cd $REMOTE_BUILD_PATH; source $REMOTE_COMPILER_PATH; make -j$BUILD_JOB_NUM; cp $REMOTE_BUILD_PATH/tsar/tsar $REMOTE_INSTALL_PATH; cd $TSAR_REMOTE_REPO; git reset --hard $REMOTE_REPO_NAME/$CURRENT_TSAR_BRANCH^; cd $IDB_REMOTE_REPO; git reset --hard $REMOTE_REPO_NAME/$CURRENT_IDB_BRANCH^"
