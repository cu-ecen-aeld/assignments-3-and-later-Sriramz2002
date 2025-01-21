#!/bin/bash


#author by SRIRAMKUMAR JAYARAMAN, 




# Function to display usage information
usage() {
    echo "Usage: $0 <directory> <search string>"
    echo "Search for files in a directory and count matching lines containing the search string."
    echo "Arguments:"
    echo "  <directory>     Directory to search in."
    echo "  <search string> String to search for in the files."
    exit 1
}



if [ "$#" -lt 2 ]; # Check if the required arguments are provided

then
    echo "Minimum 2 arguments required."
    
    usage
fi

# Assign arguments to variables
filesdir=$1
searchstr=$2

# Validate directory
if [ ! -d "$filesdir" ] || [ ! -r "$filesdir" ]; then
    echo "Eror '$filesdir' is not an readable directory."
    exit 2
fi


file_count=$(find "$filesdir" -type f ! -type l | wc -l) 
# Count the number of files ,referenced by chatgpt// asked how to count files in a specified directories and and it directories inside it a script variable for it

# Count the number of matching lines
matching_lines=$(grep -rI -- "$searchstr" "$filesdir" 2>/dev/null | wc -l) #referenced by chatgpt ,the command counts the total number of lines in all files within a specified directory (and its subdirectories) that contain the given search string, ignoring binary files. It also suppresses any errors encountered during the search.

echo "The number of files are $file_count and the number of matching lines are $matching_lines"

# Exit with appropriate status
exit 0
