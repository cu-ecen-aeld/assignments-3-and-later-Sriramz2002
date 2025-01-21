#!/bin/bash

#AUTHOR BY SRIRAMKUMAR JAYARAMAN

# Check if both arguments are provided
if [ $# -ne 2 ]; 
then
    echo "error 2 two arguments are required - writefile and writestr"
    exit 1
fi

writefile=$1
writestr=$2

# Create the directory if it does not exist

direc=$(dirname "$writefile")

mkdir -p "$direc" #create a director

# Attempt to write to the file
if ! echo "$writestr" > "$writefile"; 
then
    echo "Error!"
    echo "could not create or write to $writefile"
    exit 1
fi

echo "File $writefile created with content: $writestr"
