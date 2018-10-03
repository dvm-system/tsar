#!/bin/sh
#===--- rbuild.sh -------------- Remote Builder ----------------*- Bash -*-===#
#
#                        Traits Static Analyzer (SAPFOR)
#
# Copyright 2018 DVM System Group
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
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
# 1. Commit all changes in current branches of BCL and TSAR repositories
#    to a remote repository with name $REMOTE_REPO_NAME in a force way
#    (git add -all, git commit, git push -f).
# 2. Use ssh to establish connection with a remote system:
#    ssh $REMOTE_HOST -p$REMOTE_PORT
# 3. Stash all changes on a remote local copy of BCL, APC and TSAR repositories.
# 4. Reapply commits on top of remote repository (git reset --hard) with name
#    $REMOTE_REPO_NAME.
# 5. Enable necessary compiler (source $REMOTE_COMPILER_PATH).
# 6. Use make and specified number of jobs ($BUILD_JOB_NUM) to build project.
# 7. Install analyzer at a specified path $REMOTE_INSTALL_PATH or use
#    make install/fast if this path is empty.
# 8. Reset the index and working tree and discard all obtained changes in the
#    working tree of BCL and TSAR repositories (git reset --hard).
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
BCL_REMOTE_REPO="$SAPFOR_REMOTE_REPO/bcl"
APC_REMOTE_REPO="$SAPFOR_REMOTE_REPO/.."

SCRIPT_PATH="`dirname \"$0\"`"
SCRIPT_PATH="`(cd \"$SCRIPT_PATH\" && pwd )`"

SAPFOR_PATH="$SCRIPT_PATH/../../../.."
TSAR_PATH="$SAPFOR_PATH/analyzers/tsar/trunk"
BCL_PATH="$SAPFOR_PATH/bcl"
APC_PATH="$SAPFOR_PATH/.."

SAPFOR_PATH="`(cd \"$SAPFOR_PATH\" && pwd )`"
TSAR_PATH="`(cd \"$TSAR_PATH\" && pwd )`"
BCL_PATH="`(cd \"$BCL_PATH\" && pwd )`"
APC_PATH="`(cd \"$APC_PATH\" && pwd )`"

# Commit all changes in BCL working tree.
cd $BCL_PATH
CURRENT_BCL_BRANCH="`git rev-parse --abbrev-ref HEAD`"
echo "This is stab to perform commit if there is no other changes." > $REMOTE_REPO_NAME.stab
git add --all
git commit -m "Prepare to build."
git push $REMOTE_REPO_NAME $CURRENT_BCL_BRANCH -f
git reset $REMOTE_REPO_NAME/$CURRENT_BCL_BRANCH^
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

# Commit all changes in APC working tree.
cd $APC_PATH
CURRENT_APC_BRANCH="`git rev-parse --abbrev-ref HEAD`"
echo "This is stab to perform commit if there is no other changes." > $REMOTE_REPO_NAME.stab
git add --all
git commit -m "Prepare to build."
git push $REMOTE_REPO_NAME $CURRENT_APC_BRANCH -f
git reset $REMOTE_REPO_NAME/$CURRENT_APC_BRANCH^
rm $REMOTE_REPO_NAME.stab

# Connect and build.
SOURCE_REMOTE_COMPILER=""
if [ -n "$REMOTE_COMPILER_PATH" ]; then
  SOURCE_REMOTE_COMPILER="source $REMOTE_COMPILER_PATH;"
fi
INSTALL="make install/fast;"
if [ -n "$REMOTE_INSTALL_PATH" ]; then
  INSTALL="cp $REMOTE_BUILD_PATH/analyzers/tsar/trunk/tsar/tsar $REMOTE_INSTALL_PATH/tsar-$CURRENT_TSAR_BRANCH;"
fi
ssh $REMOTE_HOST -p$REMOTE_PORT -t "cd $BCL_REMOTE_REPO; git fetch $REMOTE_REPO_NAME; git stash; git checkout $CURRENT_BCL_BRANCH; git reset --hard $REMOTE_REPO_NAME/$CURRENT_BCL_BRANCH; cd $APC_REMOTE_REPO; git fetch $REMOTE_REPO_NAME; git stash; git checkout $CURRENT_APC_BRANCH; git reset --hard $REMOTE_REPO_NAME/$CURRENT_APC_BRANCH; cd $TSAR_REMOTE_REPO; git fetch $REMOTE_REPO_NAME; git stash; git checkout $CURRENT_TSAR_BRANCH; git reset --hard $REMOTE_REPO_NAME/$CURRENT_TSAR_BRANCH; cd $REMOTE_BUILD_PATH; $SOURCE_REMOTE_COMPILER make -j$BUILD_JOB_NUM; $INSTALL cd $TSAR_REMOTE_REPO; git reset --hard $REMOTE_REPO_NAME/$CURRENT_TSAR_BRANCH^; cd $BCL_REMOTE_REPO; git reset --hard $REMOTE_REPO_NAME/$CURRENT_BCL_BRANCH^; cd $APC_REMOTE_REPO; git reset --hard $REMOTE_REPO_NAME/$CURRENT_APC_BRANCH^"
