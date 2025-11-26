#!/bin/bash

# Color definitions
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Configuration
BASE_URL="https://magic-mount.zrlab.org/download"
UPDATE_JSON_URL="https://magic-mount.zrlab.org/version"
CHANGELOG_URL="https://magic-mount.zrlab.org/changelog"
MODULE_ID="org.zrlab.magic_mount"

# Build state
BUILD_TYPES=()
VERSION=""
VERSION_FULL=""
GIT_COMMIT=""
TEMP_DIRS=()

# Logging functions
log_info() { echo -e "${GREEN}$*${NC}" >&2; }
log_warn() { echo -e "${YELLOW}$*${NC}" >&2; }
log_error() { echo -e "${RED}$*${NC}" >&2; }
log_step() { echo -e "\n${YELLOW}[$1/$2] $3${NC}" >&2; }

# Cleanup function
cleanup() {
    local exit_code=$?
    
    log_warn "Cleaning up temporary directories..."
    
    for dir in "${TEMP_DIRS[@]}"; do
        if [ -d "$dir" ]; then
            rm -rf "$dir"
            log_info "  Removed: $dir"
        fi
    done
    
    # If script failed, show helpful message
    if [ $exit_code -ne 0 ]; then
        log_error "Build failed with exit code $exit_code"
        log_warn "All temporary files have been cleaned up"
    fi
    
    exit $exit_code
}

# Register trap for cleanup
trap cleanup EXIT INT TERM

# Usage
usage() {
    cat << EOF
Usage: $0 [OPTIONS]

OPTIONS:
    --release       Build release version only
    --debug         Build debug version only
    -h, --help      Show this help message
    (no option)     Build both versions

Version is automatically obtained from git tags.

EXAMPLES:
    $0                      # Build both release and debug
    $0 --release            # Build release only
EOF
    exit 1
}

# Parse arguments
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            --release) BUILD_TYPES+=("release"); shift ;;
            --debug) BUILD_TYPES+=("debug"); shift ;;
            -h|--help) usage ;;
            *) log_error "Unknown parameter: $1"; usage ;;
        esac
    done
    
    # Default to both if none specified
    [ ${#BUILD_TYPES[@]} -eq 0 ] && BUILD_TYPES=("release" "debug")
}

# Check prerequisites
check_prerequisites() {
    # Check git repository
    if ! git rev-parse --git-dir > /dev/null 2>&1; then
        log_error "Not in a git repository"
        exit 1
    fi
    
    # Check zig
    if ! command -v zig &> /dev/null; then
        log_error "zig is not installed"
        exit 1
    fi
    
    # Check required directories
    for dir in template src; do
        if [ ! -d "$dir" ]; then
            log_error "Required directory '$dir' not found"
            exit 1
        fi
    done
}

# Get version information from git
get_version_info() {
    local describe=$(git describe --tags --long --always --dirty 2>/dev/null)
    
    if [ -z "$describe" ]; then
        log_error "Failed to get git information"
        exit 1
    fi
    
    # Parse git describe output: v1.0.0-5-gabc1234-dirty
    if [[ "$describe" =~ ^(.+)-([0-9]+)-g([0-9a-f]+)(-dirty)?$ ]]; then
        VERSION="${BASH_REMATCH[1]}"
        local commits="${BASH_REMATCH[2]}"
        GIT_COMMIT="${BASH_REMATCH[3]}"
        local dirty="${BASH_REMATCH[4]}"
        
        if [ "$commits" = "0" ]; then
            VERSION_FULL="$VERSION"
            log_info "Building release: $VERSION"
        else
            VERSION_FULL="${VERSION}-${commits}-g${GIT_COMMIT}"
            log_warn "Building from $commits commit(s) after $VERSION"
        fi
        
        [ -n "$dirty" ] && VERSION_FULL="${VERSION_FULL}-dirty" && log_warn "Working directory is dirty"
    else
        # No tags found
        VERSION="0.0.0"
        GIT_COMMIT="$describe"
        VERSION_FULL="$describe"
        log_warn "No tags found, using commit: $describe"
    fi
}

# Generate changelog
generate_changelog() {
    local file="build/changelog.md"
    local prev_tag=$(git describe --tags --abbrev=0 HEAD^ 2>/dev/null)
    
    cat > "$file" << EOF
# Changelog

## Version $VERSION_FULL ($(date +%Y-%m-%d))

### Changes

EOF

    if [ -n "$prev_tag" ]; then
        echo "Changes since $prev_tag:" >> "$file"
        echo "" >> "$file"
        git log --pretty=format:"- %s (%h)" "$prev_tag"..HEAD >> "$file"
        echo -e "\n\n---" >> "$file"
        echo "Full Changelog: https://github.com/7a72/meta-magic_mount/compare/${prev_tag}...${VERSION}" >> "$file"
    else
        echo "Initial release" >> "$file"
        echo "" >> "$file"
        git log --pretty=format:"- %s (%h)" >> "$file"
    fi
}

# Generate version.json for release builds
generate_update_json() {
    local output_name=$1
    local version_code=$2
    local file="build/version.json"
    
    cat > "$file" << EOF
{
    "versionCode": $version_code,
    "version": "$VERSION",
    "zipUrl": "$BASE_URL/$VERSION/$output_name",
    "changelog": "$CHANGELOG_URL"
}
EOF
}

# Build binaries
build_binaries() {
    local build_type=$1
    
    log_step 2 8 "Building binaries ($build_type)"
    
    cd src || return 1
    make clean > /dev/null 2>&1
    
    if ! make "$build_type" VERSION="$VERSION_FULL" >&2; then
        cd ..
        log_error "Failed to build binaries"
        return 1
    fi
    
    cd ..
    log_info "Binaries built successfully"
}

# Configure module files
configure_module() {
    local build_dir=$1
    local build_type=$2
    local version_code=$3
    
    log_step 3 8 "Configuring module files"
    
    # Configure module.prop
    local module_prop="$build_dir/module.prop"
    if [ ! -f "$module_prop" ]; then
        log_error "module.prop not found"
        return 1
    fi
    
    local module_version="$VERSION_FULL"
    
    sed -i "s|^id=.*|id=$MODULE_ID|" "$module_prop"
    sed -i "s|^version=.*|version=$module_version|" "$module_prop"
    sed -i "s|^versionCode=.*|versionCode=$version_code|" "$module_prop"
    sed -i "s|^updateJson=.*|updateJson=$UPDATE_JSON_URL|" "$module_prop"
    
    log_info "module.prop configured ($module_version, code $version_code)"
}

# Generate checksums for all files
generate_checksums() {
    local build_dir=$1
    
    log_step 5 8 "Generating checksums"
    
    local checksum_file="$build_dir/checksums"
    
    # Generate checksums (exclude checksums file itself if it exists)
    if ! (cd "$build_dir" && \
        find . -type f ! -name "checksums" -print0 | \
        LC_ALL=C sort -z | \
        xargs -0 sha256sum | \
        sed 's|^\./||' > checksums); then
        log_error "Failed to generate checksums"
        return 1
    fi
    
    local count=$(wc -l < "$checksum_file")
    log_info "Generated checksums for $count files"
}

# Normalize timestamps for reproducible builds
normalize_timestamps() {
    local build_dir=$1
    
    log_step 6 8 "Normalizing timestamps for reproducible build"
    
    # Use git commit timestamp for reproducibility
    local git_timestamp=$(git log -1 --format=%ct 2>/dev/null)
    
    if [ -n "$git_timestamp" ]; then
        # Try GNU date first, then BSD date (macOS)
        local timestamp=$(date -d "@$git_timestamp" '+%Y%m%d%H%M.%S' 2>/dev/null)
        if [ -z "$timestamp" ]; then
            # macOS compatibility
            timestamp=$(date -r "$git_timestamp" '+%Y%m%d%H%M.%S' 2>/dev/null)
        fi
        
        if [ -z "$timestamp" ]; then
            log_error "Failed to convert git timestamp"
            return 1
        fi
        
        # Show human-readable commit time
        local human_time=$(date -d "@$git_timestamp" '+%Y-%m-%d %H:%M:%S %Z' 2>/dev/null)
        if [ -z "$human_time" ]; then
            human_time=$(date -r "$git_timestamp" '+%Y-%m-%d %H:%M:%S %Z' 2>/dev/null)
        fi
        log_info "Using git commit time: $human_time"
    else
        timestamp=$(date '+%Y%m%d%H%M.%S')
        log_warn "Git timestamp not available, using current time"
    fi
    
    if ! find "$build_dir" -exec touch -m -t "$timestamp" {} +; then
        log_error "Failed to normalize timestamps"
        return 1
    fi
    
    log_info "All timestamps normalized to: $timestamp"
}

# Build single type
build_single_type() {
    local build_type=$1
    local build_dir="build/${build_type}_temp"
    local version_code=$(git rev-list --count HEAD)
    local output_name="meta-magic_mount-${VERSION_FULL}-${build_type}.zip"
    
    # Register this temp directory for cleanup
    TEMP_DIRS+=("$build_dir")
    TEMP_DIRS+=("src/bin")
    
    log_info ""
    log_info "========================================"
    log_info "Building $build_type version"
    log_info "Version: $VERSION_FULL"
    log_info "========================================"
    
    # Clean and create build directory
    rm -rf "$build_dir"
    mkdir -p "$build_dir" || {
        log_error "Failed to create build directory: $build_dir"
        return 1
    }
    
    # Step 1: Copy template
    log_step 1 8 "Copying template"
    if ! cp -r template/* "$build_dir/"; then
        log_error "Failed to copy template"
        return 1
    fi
    
    # Step 2: Build binaries
    build_binaries "$build_type" || return 1
    
    # Step 3: Configure module
    configure_module "$build_dir" "$build_type" "$version_code" || return 1
    
    # Step 4: Copy binaries
    log_step 4 8 "Copying binaries"
    if [ ! -d "src/bin" ]; then
        log_error "src/bin not found"
        return 1
    fi
    if ! cp -r src/bin "$build_dir/"; then
        log_error "Failed to copy binaries"
        return 1
    fi
    
    # Step 5: Generate checksums
    generate_checksums "$build_dir" || return 1
    
    # Step 6: Normalize timestamps
    normalize_timestamps "$build_dir" || return 1
    
    # Step 7: Package
    log_step 7 8 "Creating package"
    if ! (cd "$build_dir" && zip -qr "../../build/$output_name" ./*); then
        log_error "Failed to create package"
        return 1
    fi
    
    # Step 8: Generate misc for release
    if [ "$build_type" = "release" ]; then
        log_step 8 8 "Generating misc"
        generate_changelog
        generate_update_json "$output_name" "$version_code"
    else
        log_step 8 8 "Skipping misc (debug build)"
    fi
    
    local size=$(du -h "build/$output_name" | cut -f1)
    log_info ""
    log_info "========================================="
    log_info "Build complete: $output_name ($size)"
    log_info "========================================="
    
    artifact="$output_name"
}

# Main build process
main() {
    parse_args "$@"
    check_prerequisites
    get_version_info
    
    # Setup build directory
    mkdir -p build
    
    # Build each type
    local success=0
    local failed=0
    local built_files=()
    
    for build_type in "${BUILD_TYPES[@]}"; do
        if build_single_type "$build_type"; then
            ((success++))
            built_files+=("$artifact")
        else
            ((failed++))
            log_error "Failed to build $build_type"
        fi
    done
    
    # Print summary
    log_info ""
    log_info "========================================"
    log_info "Build Summary"
    log_info "========================================"
    log_info "Version: $VERSION_FULL"
    log_info "Commit: $GIT_COMMIT"
    log_info "Success: $success | Failed: $failed"
    log_info "----------------------------------------"
    log_info "Generated files:"
    if [[ " ${BUILD_TYPES[*]} " =~ " release " ]]; then
        log_info "  build/changelog.md"
        log_info "  build/version.json"
    fi
    for file in "${built_files[@]}"; do
        local size=$(du -h "build/$file" | cut -f1)
        log_info "  build/$file ($size)"
    done
    log_info "========================================"
    
    [ $failed -gt 0 ] && exit 1
    exit 0
}

main "$@"
