#!/usr/bin/env bash
set -euo pipefail
IFS=$'\n\t'

# --- Configuration ---
readonly IMAGE_TAGS_FILE=".gitlab-ci/conditional-build-image-tags.yml"

# --- Helper Functions ---

# Print usage/help text.
show_help() {
    cat <<EOF
This script manages container image tags for CI builds.

Usage:
  $0 --all                     Update all component tags in ${IMAGE_TAGS_FILE}
  $0 --all-but <component>     Update all component tags except for <component>
  $0 --component <component>   Update tag for a specific component
  $0 --list                    List all available components
  $0 --check [<component> ...] Check given components
  $0 --help                    Show this help message

Examples:
  List available components ready for structural tagging:
    $0 --list

  Update all components ready for structural tagging:
    $0 --all

  Update all except 'skqp' component:
    $0 --all-but skqp

  Update only 'angle' component:
    $0 --component angle

  Check if 'deqp-runner' and 'deqp' components build:
    $0 --check deqp-runner deqp
EOF
    exit 1
}

check_requirements() {
    if ! command -v yq &>/dev/null; then
        _error_msg "yq command not found. Please install mikefarah/yq to update YAML files."
        exit 1
    fi
}

# Loads environment variables from a YAML file (only lines with _TAG: are processed).
load_env_vars() {
    local env_file="$1"
    if [[ -f "$env_file" ]]; then
		GREP_OUTPUT=$(grep '_TAG:' "$env_file")
		if (( $? != 0 )); then
			echo "No _TAG: found in $env_file"
			return
		fi
        while IFS=":" read -r key value; do
            # Skip empty lines and comments.
            [[ -z "$key" || "$key" =~ ^[[:space:]]*# ]] && continue
			# Trim leading/trailing whitespace and quotes from key.
			key=$(echo "$key" | sed -e 's/^[[:space:]"'\'']*//' -e 's/[[:space:]"'\'']*$//')
			# Trim leading/trailing whitespace and quotes from value.
            value=$(echo "$value" | sed -e 's/^[[:space:]"'\'']*//' -e 's/[[:space:]"'\'']*$//')
            # Remove the "CONDITIONAL_BUILD_" prefix from the key, if present.
            key="${key#CONDITIONAL_BUILD_}"
            export "$key=$value"
        done < <(echo "$GREP_OUTPUT")
    fi
}

# Set up a dummy test environment to satisfy build-* scripts' set -u checks.
run_setup_test_env() {
    export DEQP_API="dummy"
    export DEQP_TARGET="dummy"
    CI_JOB_STARTED_AT=$(date -u +%Y-%m-%dT%H:%M:%S%z)
    CI_PROJECT_DIR=$(git rev-parse --show-toplevel)
    export CI_JOB_STARTED_AT CI_PROJECT_DIR

    if [[ -f ".gitlab-ci/setup-test-env.sh" ]]; then
        . .gitlab-ci/setup-test-env.sh
    else
        _error_msg "Test environment setup script .gitlab-ci/setup-test-env.sh not found."
        exit 1
    fi
}

# --- Core Functions ---

# Find available components by intersecting candidates from the GitLab CI YAML
# with available build-*.sh scripts. Outputs one component name per line.
find_components() {
    local candidate_file=".gitlab-ci/container/gitlab-ci.yml"
    if [[ ! -f "$candidate_file" ]]; then
        _error_msg "File not found: $candidate_file"
        exit 1
    fi

    # Extract candidate component names from the GitLab CI file.
    mapfile -t candidates < <(grep -oP '(?<=\.container-builds-)[a-z0-9_-]+' "$candidate_file" | sort -u)
    echo "Candidates found in ${candidate_file}: $(IFS=,; echo "${candidates[*]}")" >&2

    # Extract component names from available build scripts.
    local script
    local -a build_scripts=()
    for script in .gitlab-ci/container/build-*.sh; do
        if [[ -f "$script" ]]; then
            local comp_name
            comp_name=$(basename "$script" | sed -E 's/^build-([a-z0-9_-]+)\.sh$/\1/')
            build_scripts+=("$comp_name")
        fi
    done
    # Sort and remove duplicates.
    IFS=$'\n' read -r -d '' -a unique_build_scripts < <(printf "%s\n" "${build_scripts[@]}" | sort -u && printf '\0')

    # Compute intersection between candidates and build script components.
    local comp
    for comp in "${candidates[@]}"; do
        if printf "%s\n" "${unique_build_scripts[@]}" | grep -qx "$comp"; then
            echo "$comp"
        fi
    done
}

# Display a friendly error message based on the contents of the given error file.
nice_err_message() (
    local err_file="$1"
    local unbound_var
    unbound_var=$(grep -oP '([A-Z_]+)(?=: unbound variable)' "$err_file" || true)
    if [[ -n "$unbound_var" ]]; then
        _error_msg "Please set the variable ${unbound_var}."
        exit 1
    fi
    echo "Please check the error message and fix it:" >&2
    cat "$err_file" >&2
    exit 1
)

# Run the build script for a specific component.
# On success, the build script's last output line (i.e. the tag value) is printed.
run_component() {
    local component="$1"
    local tag_var
    # Convert the component name to upper-case and replace hyphens with underscores.
    tag_var="$(echo "$component" | tr '[:lower:]-' '[:upper:]_')_TAG"
    local build_script=".gitlab-ci/container/build-${component}.sh"

    if [[ ! -f "$build_script" ]]; then
        _error_msg "Build script ${build_script} not found for component ${component}."
        return 1
    fi

    if ! grep -qiE "$tag_var" "$build_script"; then
        _error_msg "Skipping ${build_script} because it does not use ${tag_var} in the tag check."
        return 1
    fi

    err_file=$(mktemp)
    out_file=$(mktemp)
    # Ensure temporary files are removed when this function exits.
    trap 'rm -f "$err_file" "$out_file"' RETURN

    if bash "$build_script" > "$out_file" 2>"$err_file"; then
        # The tag value is assumed to be the last line of output.
        local tag_value
        tag_value=$(tail -n 1 "$out_file")
        echo "$tag_value"
        return 0
	fi

	if nice_err_message "$err_file"; then
		exit 1
	fi
	echo "Failed to check ${component}. Please check the error message and fix it."
	echo "BASH_COMMAND: bash ${build_script}"
	cat "$err_file"
	cat "$out_file"
	return 1
}

# Update the image tags file by setting the variable for the given component.
update_image_tags() {
    check_requirements  # needs yq
    local component="$1"
    local tag_value="$2"
    # The environment variable name in the YAML is prefixed with CONDITIONAL_BUILD_.
    local full_tag_var
    full_tag_var="CONDITIONAL_BUILD_$(echo "$component" | tr '[:lower:]-' '[:upper:]_')_TAG"
    echo -ne "Tagging ${full_tag_var}\r"
    yq eval ".variables.${full_tag_var} = \"${tag_value}\"" -i "$IMAGE_TAGS_FILE"
    yq eval -i 'sort_keys(.variables)' "$IMAGE_TAGS_FILE"
}

# Run tag updates for all components.
run_all() {
    local comp
    local -a components
    mapfile -t components < <(find_components)
    export NEW_TAG_DRY_RUN=1
    for comp in "${components[@]}"; do
        echo "Updating ${comp} tag"
        if tag_value=$(run_component "$comp"); then
            update_image_tags "$comp" "$tag_value"
        fi
    done
    # Clear the output line.
    printf "\033[K"
}

# Run tag updates for all components except for one specified component.
run_all_but() {
    local exclude="$1"
    local comp
    local -a components_to_update=()
    export NEW_TAG_DRY_RUN=1
    while IFS= read -r comp; do
        if [[ "$comp" != "$exclude" ]]; then
            components_to_update+=("$comp")
        fi
    done < <(find_components)

    for comp in "${components_to_update[@]}"; do
        echo "Checking ${comp}"
        if tag_value=$(run_component "$comp"); then
            update_image_tags "$comp" "$tag_value"
        fi
    done
    printf "\033[K"
}

# Check one or more components without updating the YAML file.
check_components() {
	if [[ "${CI:-false}" = "false" ]]; then
		if [[ -f "$IMAGE_TAGS_FILE" ]]; then
			load_env_vars "$IMAGE_TAGS_FILE"
		fi
	fi

    # Ensure that NEW_TAG_DRY_RUN is not set and set CI_NOT_BUILDING_ANYTHING.
    unset NEW_TAG_DRY_RUN || true
    export CI_NOT_BUILDING_ANYTHING=1

    local comp
    for comp in "$@"; do
        run_component "$comp"
    done
}

# --- Argument Parsing ---

parse_args() {
    if [[ "$#" -eq 0 ]]; then
        show_help
    fi

    case "$1" in
        --all)
            run_all
            ;;
        --all-but)
            if [[ -z "${2-}" ]]; then
                _error_msg "--all-but requires a component name."
                exit 1
            fi
            run_all_but "$2"
            ;;
        --component)
            if [[ -z "${2-}" ]]; then
                _error_msg "--component requires a component name."
                exit 1
            fi
			export NEW_TAG_DRY_RUN=1
            if tag_value=$(run_component "$2"); then
                update_image_tags "$2" "$tag_value"
            fi
            ;;
        --check)
            shift
            check_components "$@"
            ;;
        --list)
            local -a comps
            mapfile -t comps < <(find_components)
            echo "Detected components: $(IFS=,; echo "${comps[*]}")"
            ;;
        --help)
            show_help
            ;;
        *)
            show_help
            ;;
    esac
}

# --- Main Execution ---

run_setup_test_env
parse_args "$@"
