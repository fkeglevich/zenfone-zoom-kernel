#!/bin/sh

# Compresses a directory to a TWRP flashable zip using all available cpu cores
# Example:
# ./twrp-flashable.sh /twrp/flashable/directory/full/path

INPUT_DIR=$1
OUTPUT_FILENAME=$INPUT_DIR.zip
echo "Compressing $INPUT_DIR to $OUTPUT_FILENAME file!"
7z a -tzip -mmt=on $OUTPUT_FILENAME $INPUT_DIR/*
