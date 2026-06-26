#!/usr/bin/env python3
import sys
import os
import struct

def build_package(src_dir, dest_path, name, version, desc, depends):
    # Get all files recursively in src_dir
    files_list = []
    for root, dirs, files in os.walk(src_dir):
        for file in files:
            full_path = os.path.join(root, file)
            rel_path = os.path.relpath(full_path, src_dir)
            # Ensure path uses forward slashes and starts with /
            target_path = "/" + rel_path.replace(os.path.sep, "/")
            files_list.append((full_path, target_path))

    num_files = len(files_list)
    magic = b"WPKG"
    
    # Pack header: magic(4s), name(64s), version(16s), desc(128s), depends(128s), num_files(I)
    name_b = name.encode('utf-8')[:63].ljust(64, b'\x00')
    version_b = version.encode('utf-8')[:15].ljust(16, b'\x00')
    desc_b = desc.encode('utf-8')[:127].ljust(128, b'\x00')
    depends_b = depends.encode('utf-8')[:127].ljust(128, b'\x00')
    
    # Struct format: 4s, 64s, 16s, 128s, 128s, I
    header = struct.pack("<4s64s16s128s128sI", magic, name_b, version_b, desc_b, depends_b, num_files)
    
    # Calculate starting offset for file data
    # Header size = 4 + 64 + 16 + 128 + 128 + 4 = 344
    # File headers size = num_files * 136 (WpkgFileHeader = 128s, I, I)
    current_offset = 344 + num_files * 136
    
    file_headers = []
    file_data_blocks = []
    
    for full_path, target_path in files_list:
        with open(full_path, 'rb') as f:
            data = f.read()
        size = len(data)
        
        # Pack WpkgFileHeader: path(128s), size(I), offset(I)
        path_b = target_path.encode('utf-8')[:127].ljust(128, b'\x00')
        file_header = struct.pack("<128sII", path_b, size, current_offset)
        file_headers.append(file_header)
        
        file_data_blocks.append(data)
        current_offset += size
        
    with open(dest_path, 'wb') as out:
        out.write(header)
        for fh in file_headers:
            out.write(fh)
        for fd in file_data_blocks:
            out.write(fd)
            
    print(f"Created package {dest_path} with {num_files} files.")

if __name__ == '__main__':
    if len(sys.argv) < 6:
        print("Usage: python pkg_builder.py <src_dir> <dest.wpkg> <name> <version> <desc> [depends]")
        sys.exit(1)
        
    src_dir = sys.argv[1]
    dest_path = sys.argv[2]
    name = sys.argv[3]
    version = sys.argv[4]
    desc = sys.argv[5]
    depends = sys.argv[6] if len(sys.argv) > 6 else ""
    
    build_package(src_dir, dest_path, name, version, desc, depends)
