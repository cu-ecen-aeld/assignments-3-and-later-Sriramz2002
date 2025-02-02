#!/bin/sh
# Tester script for assignment 1 and assignment 2
# Author: Sriramkumar Jayaraman

set -e
set -u

NUMFILES=10
WRITESTR=AELD_IS_FUN
WRITEDIR=/tmp/aeld-data
username=$(cat conf/username.txt)

# Check if arguments are provided
if [ $# -lt 3 ]; then
    echo "Using default value ${WRITESTR} for string to write"
    if [ $# -lt 1 ]; then
        echo "Using default value ${NUMFILES} for number of files to write"
    else
        NUMFILES=$1
    fi
else
    NUMFILES=$1
    WRITESTR=$2
    WRITEDIR=/tmp/aeld-data/$3
fi

MATCHSTR="The number of files are ${NUMFILES} and the number of matching lines are ${NUMFILES}"

# Clean previous build artifacts
echo "Cleaning previous build artifacts"
#make clean

# Compile the writer application using native compilation
echo "Compiling writer application"
#make
if [ $? -ne 0 ]; then
    echo "Failed to compile writer application"
    exit 1
fi

# Write files using the writer utility
echo "Writing ${NUMFILES} files containing string ${WRITESTR} to ${WRITEDIR}"
rm -rf "${WRITEDIR}"

# Create WRITEDIR if not assignment1
assignment=$(cat ../conf/assignment.txt)

if [ "$assignment" != "assignment1" ]; then
    mkdir -p "$WRITEDIR"

    if [ -d "$WRITEDIR" ]; then
        echo "$WRITEDIR created"
    else
        echo "Failed to create $WRITEDIR"
        exit 1
    fi
fi

for i in $(seq 1 $NUMFILES); do
    ./writer "$WRITEDIR/${username}$i.txt" "$WRITESTR"
done

# Run finder.sh and capture output
OUTPUTSTRING=$(./finder.sh "$WRITEDIR" "$WRITESTR")

# Clean up temporary directories
rm -rf /tmp/aeld-data

# Verify output
set +e
echo "$OUTPUTSTRING" | grep "$MATCHSTR"
if [ $? -eq 0 ]; then
    echo "success"
    exit 0
else
    echo "failed: expected ${MATCHSTR} in ${OUTPUTSTRING} but instead found"
    exit 1
fi

