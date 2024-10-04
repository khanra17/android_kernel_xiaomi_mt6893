#!/bin/bash

# Get the current project directory using pwd
PROJECT_ROOT=$(pwd)

# Create the 'cleanup' directory if it doesn't exist
if [ ! -d "cleanup" ]; then
    mkdir cleanup
    echo "Directory 'cleanup' created."
else
    echo "Directory 'cleanup' already exists."
fi

# Define the access log file path
ACCESS_LOG_FILE="$PROJECT_ROOT/cleanup/all_accessed_files.log"

# Build the kernel
build_kernel() {
    # Monitor all file accesses in the project directory
    inotifywait -mrq -e open --format '%w%f' "$PROJECT_ROOT" > "$ACCESS_LOG_FILE" &

    # Capture the PID of inotifywait process to stop it after the build
    INOTIFY_PID=$!

    # Run the build
    ./build.sh

    # Stop the inotifywait process
    kill $INOTIFY_PID
}

# Mark unused files based on the access log
mark_unused_files() {
    echo "Identifying files not accessed during build..."

    # List all files in the project directory (relative paths)
    find "$PROJECT_ROOT" -type f > cleanup/all_files.txt

    # Sort and uniq the accessed files list
    sort "$ACCESS_LOG_FILE" | uniq > cleanup/accessed_files.txt

    # Identify files not accessed during the build
    grep -Fxv -f cleanup/accessed_files.txt cleanup/all_files.txt > cleanup/unused_files.txt
}

delete_unused_files() {
    echo "Deleting unused files..."

    while read -r file; do
        if [ -f "$file" ]; then
            rm "$file"
            echo "Deleted $file"
        fi
    done < cleanup/unused_files.txt

    echo "Clean up complete."
}

build_kernel
mark_unused_files
# delete_unused_files
