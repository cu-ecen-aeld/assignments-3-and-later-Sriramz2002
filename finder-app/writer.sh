#!/bin/bash

# Check if both arguments are provided
if [ $# -ne 2 ]; then
    echo "Error: Two arguments are required - writefile and writestr"
    exit 1
fi

writefile=$1
writestr=$2

# Create the directory if it does not exist
dir=$(dirname "$writefile")
mkdir -p "$dir"

# Attempt to write to the file
if ! echo "$writestr" > "$writefile"; then
    echo "Error: Could not create or write to $writefile"
    exit 1
fi

# Success
echo "File $writefile created with content: $writestr"
