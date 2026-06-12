#!/usr/bin/env python3
import os
import sys
import re
import shutil
import struct
from datetime import datetime

# Logging setup - only errors and summaries are logged
LOG_FILE = "/home/pi/piTrove/logs/organizer.log"
os.makedirs(os.path.dirname(LOG_FILE), exist_ok=True)

def log_error(err_code, message):
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    log_entry = f"[{timestamp}] [ERROR] {err_code}: {message}\n"
    sys.stderr.write(f"ERROR {err_code}: {message}\n")
    try:
        with open(LOG_FILE, "a") as f:
            f.write(log_entry)
    except Exception as e:
        sys.stderr.write(f"Failed to write to log file: {str(e)}\n")

# Pure Python JPEG EXIF reader to extract DateTimeOriginal without external dependencies
def get_exif_date(filepath):
    try:
        with open(filepath, 'rb') as f:
            # Check JPEG SOI
            if f.read(2) != b'\xff\xd8':
                return None
            
            while True:
                marker_header = f.read(2)
                if len(marker_header) < 2:
                    break
                marker, = struct.unpack('>H', marker_header)
                if marker == 0xffda:  # Start of Scan (SOS), stop scanning
                    break
                
                size_bytes = f.read(2)
                if len(size_bytes) < 2:
                    break
                size, = struct.unpack('>H', size_bytes)
                
                # Check APP1 (EXIF marker)
                if marker == 0xffe1:
                    data = f.read(size - 2)
                    if len(data) < 6 or data[0:6] != b'Exif\x00\x00':
                        continue
                    
                    # TIFF Header
                    tiff_offset = 6
                    byte_order = data[tiff_offset : tiff_offset+2]
                    if byte_order == b'II':
                        endian = '<'
                    elif byte_order == b'MM':
                        endian = '>'
                    else:
                        continue
                    
                    # Check magic number 42
                    magic = struct.unpack(endian + 'H', data[tiff_offset+2 : tiff_offset+4])[0]
                    if magic != 42:
                        continue
                    
                    ifd_offset = struct.unpack(endian + 'I', data[tiff_offset+4 : tiff_offset+8])[0]
                    
                    # Parse IFD0
                    curr_offset = tiff_offset + ifd_offset
                    if curr_offset >= len(data):
                        continue
                    
                    num_entries = struct.unpack(endian + 'H', data[curr_offset : curr_offset+2])[0]
                    curr_offset += 2
                    
                    exif_sub_ifd_offset = None
                    for _ in range(num_entries):
                        if curr_offset + 12 > len(data):
                            break
                        tag, field_type, count, val_offset = struct.unpack(endian + 'HHII', data[curr_offset : curr_offset+12])
                        curr_offset += 12
                        
                        # Tag 0x8769 is ExifOffset (Exif SubIFD)
                        if tag == 0x8769:
                            exif_sub_ifd_offset = val_offset
                            break
                    
                    if exif_sub_ifd_offset is not None:
                        sub_offset = tiff_offset + exif_sub_ifd_offset
                        if sub_offset < len(data):
                            num_sub_entries = struct.unpack(endian + 'H', data[sub_offset : sub_offset+2])[0]
                            sub_offset += 2
                            for _ in range(num_sub_entries):
                                if sub_offset + 12 > len(data):
                                    break
                                tag, field_type, count, val_offset = struct.unpack(endian + 'HHII', data[sub_offset : sub_offset+12])
                                sub_offset += 12
                                
                                # Tag 0x9003 is DateTimeOriginal
                                if tag == 0x9003:
                                    string_offset = tiff_offset + val_offset
                                    if string_offset + count <= len(data):
                                        date_str = data[string_offset : string_offset + count - 1].decode('ascii', errors='ignore')
                                        # Format: YYYY:MM:DD HH:MM:SS
                                        match = re.match(r'^(\d{4}):(\d{2}):(\d{2})', date_str)
                                        if match:
                                            return f"{match.group(1)}-{match.group(2)}-{match.group(3)}"
                    break
                else:
                    # Skip segment data
                    f.seek(size - 2, 1)
    except Exception as e:
        log_error("E504", f"EXIF parsing crashed on {filepath}: {str(e)}")
    return None

# Extract date from filename patterns
def get_date_from_filename(filename):
    # Matches YYYY-MM-DD or YYYYMMDD
    match = re.search(r'(\d{4})[-_]?(\d{2})[-_]?(\d{2})', filename)
    if match:
        year, month, day = match.group(1), match.group(2), match.group(3)
        # Validate values
        if 1980 <= int(year) <= 2040 and 1 <= int(month) <= 12 and 1 <= int(day) <= 31:
            return f"{year}-{month}-{day}"
    return None

# Main Reorganization Routine
def organize_archive(root_dir, in_place=False):
    if not os.path.isdir(root_dir):
        log_error("E501", f"Target directory does not exist: {root_dir}")
        sys.exit(1)
        
    scanned_count = 0
    organized_count = 0
    error_count = 0
    
    # Supported formats
    image_exts = {'.jpg', '.jpeg', '.png', '.tiff', '.tif', '.webp', '.heic', '.heif', '.bmp'}
    video_exts = {'.mp4', '.m4v', '.mov', '.avi', '.mkv', '.hevc'}
    
    # We will traverse files and record operations first to avoid changing state while traversing.
    moves_to_perform = []
    
    # Pattern to detect if a file is already organized in Photos/YYYY-MM/YYYY-MM-DD_...
    org_pattern = re.compile(r'/(Photos|Videos)/\d{4}-\d{2}/\d{4}-\d{2}-\d{2}_')
    
    for dirpath, _, filenames in os.walk(root_dir):
        # Skip if already in organized paths to prevent double processing/loops
        rel_path = os.path.relpath(dirpath, root_dir)
        if not in_place and org_pattern.search("/" + rel_path.replace(os.sep, "/") + "/"):
            continue
            
        for filename in filenames:
            ext = os.path.splitext(filename)[1].lower()
            if ext not in image_exts and ext not in video_exts:
                continue
                
            src_path = os.path.join(dirpath, filename)
            scanned_count += 1
            
            # 1. Determine Date
            date_str = None
            if ext in {'.jpg', '.jpeg'}:
                date_str = get_exif_date(src_path)
            
            if not date_str:
                date_str = get_date_from_filename(filename)
                
            if not date_str:
                # Fallback to filesystem mtime
                try:
                    mtime = os.stat(src_path).st_mtime
                    date_str = datetime.fromtimestamp(mtime).strftime("%Y-%m-%d")
                except Exception as e:
                    log_error("E503", f"Could not retrieve timestamp for {src_path}: {str(e)}")
                    error_count += 1
                    continue
            
            # Parse Year and Month
            parts = date_str.split('-')
            year_month = f"{parts[0]}-{parts[1]}"
            
            # 2. Determine Destination folder
            if in_place:
                dest_dir = dirpath
            else:
                media_subfolder = "Photos" if ext in image_exts else "Videos"
                dest_dir = os.path.join(root_dir, media_subfolder, year_month)
            
            # 3. Handle filename prefix and conflicts
            base_name = os.path.splitext(filename)[0]
            clean_base = re.sub(r'^\d{4}-\d{2}-\d{2}_', '', base_name)
            
            new_filename = f"{date_str}_{clean_base}{ext}"
            dest_path = os.path.join(dest_dir, new_filename)
            
            # If src_path is already identical to dest_path, skip
            if os.path.abspath(src_path) == os.path.abspath(dest_path):
                continue
                
            # Handle conflict resolution
            counter = 1
            while os.path.exists(dest_path):
                new_filename = f"{date_str}_{clean_base}_{counter}{ext}"
                dest_path = os.path.join(dest_dir, new_filename)
                counter += 1
                
            moves_to_perform.append((src_path, dest_dir, dest_path))
            
    # Perform moves
    for src_path, dest_dir, dest_path in moves_to_perform:
        try:
            # Ensure destination directory exists
            if not os.path.exists(dest_dir):
                try:
                    os.makedirs(dest_dir, exist_ok=True)
                except Exception as e:
                    log_error("E502", f"Failed to create directory {dest_dir}: {str(e)}")
                    error_count += 1
                    continue
            
            # Capture original timestamps
            stat = os.stat(src_path)
            orig_atime = stat.st_atime
            orig_mtime = stat.st_mtime
            
            # Move/Rename file
            shutil.move(src_path, dest_path)
            
            # Restore timestamps on destination
            os.utime(dest_path, (orig_atime, orig_mtime))
            organized_count += 1
        except Exception as e:
            log_error("E503", f"Failed to move {src_path} to {dest_path}: {str(e)}")
            error_count += 1
            
    # Print summary
    print(f"\n[✓] Reorganization complete.")
    print(f"    - Total files scanned: {scanned_count}")
    print(f"    - Files successfully reorganized: {organized_count}")
    print(f"    - Errors encountered: {error_count} (Logged to {LOG_FILE})")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: organize.py <archive folder> [--in-place]")
        sys.exit(1)
        
    in_place = "--in-place" in sys.argv
    # Filter out --in-place flag
    args = [arg for arg in sys.argv[1:] if arg != "--in-place"]
    if not args:
        print("Usage: organize.py <archive folder> [--in-place]")
        sys.exit(1)
        
    organize_archive(args[0], in_place=in_place)
