#!/bin/bash

for var in \
    ASAN_OPTIONS \
    BASE_SYSTEM_FORK_HOST_PREFIX \
    BASE_SYSTEM_MAINLINE_HOST_PREFIX \
    CI_COMMIT_BRANCH \
    CI_COMMIT_REF_NAME \
    CI_COMMIT_TITLE \
    CI_JOB_ID \
    CI_JOB_JWT_FILE \
    CI_JOB_NAME \
    CI_JOB_URL \
    CI_MERGE_REQUEST_SOURCE_BRANCH_NAME \
    CI_MERGE_REQUEST_TITLE \
    CI_NODE_INDEX \
    CI_NODE_TOTAL \
    CI_PAGES_DOMAIN \
    CI_PIPELINE_ID \
    CI_PIPELINE_URL \
    CI_PROJECT_DIR \
    CI_PROJECT_NAME \
    CI_PROJECT_PATH \
    CI_PROJECT_ROOT_NAMESPACE \
    CI_RUNNER_DESCRIPTION \
    CI_SERVER_URL \
    CROSVM_GALLIUM_DRIVER \
    CROSVM_GPU_ARGS \
    DEQP_BIN_DIR \
    DEQP_CASELIST_FILTER \
    DEQP_CASELIST_INV_FILTER \
    DEQP_CONFIG \
    DEQP_EXPECTED_RENDERER \
    DEQP_FRACTION \
    DEQP_HEIGHT \
    DEQP_RESULTS_DIR \
    DEQP_RUNNER_OPTIONS \
    DEQP_SUITE \
    DEQP_TEMP_DIR \
    DEQP_VARIANT \
    DEQP_VER \
    DEQP_WIDTH \
    DEVICE_NAME \
    DRIVER_NAME \
    EGL_PLATFORM \
    ETNA_MESA_DEBUG \
    FDO_CI_CONCURRENT \
    FDO_UPSTREAM_REPO \
    FD_MESA_DEBUG \
    FLAKES_CHANNEL \
    FREEDRENO_HANGCHECK_MS \
    GALLIUM_DRIVER \
    GALLIVM_PERF \
    GPU_VERSION \
    GTEST \
    GTEST_FAILS \
    GTEST_FRACTION \
    GTEST_RESULTS_DIR \
    GTEST_RUNNER_OPTIONS \
    GTEST_SKIPS \
    HWCI_FREQ_MAX \
    HWCI_KERNEL_MODULES \
    HWCI_KVM \
    HWCI_START_XORG \
    HWCI_TEST_SCRIPT \
    IR3_SHADER_DEBUG \
    JOB_ARTIFACTS_BASE \
    JOB_RESULTS_PATH \
    JOB_ROOTFS_OVERLAY_PATH \
    KERNEL_IMAGE_BASE_URL \
    KERNEL_IMAGE_NAME \
    LD_LIBRARY_PATH \
    LP_NUM_THREADS \
    MESA_BASE_TAG \
    MESA_BUILD_PATH \
    MESA_DEBUG \
    MESA_GLES_VERSION_OVERRIDE \
    MESA_GLSL_VERSION_OVERRIDE \
    MESA_GL_VERSION_OVERRIDE \
    MESA_IMAGE \
    MESA_IMAGE_PATH \
    MESA_IMAGE_TAG \
    MESA_TEMPLATES_COMMIT \
    MESA_VK_IGNORE_CONFORMANCE_WARNING \
    MESA_SPIRV_LOG_LEVEL \
    MINIO_HOST \
    MINIO_RESULTS_UPLOAD \
    NIR_DEBUG \
    PAN_I_WANT_A_BROKEN_VULKAN_DRIVER \
    PAN_MESA_DEBUG \
    PIGLIT_FRACTION \
    PIGLIT_NO_WINDOW \
    PIGLIT_OPTIONS \
    PIGLIT_PLATFORM \
    PIGLIT_PROFILES \
    PIGLIT_REPLAY_ARTIFACTS_BASE_URL \
    PIGLIT_REPLAY_DESCRIPTION_FILE \
    PIGLIT_REPLAY_DEVICE_NAME \
    PIGLIT_REPLAY_EXTRA_ARGS \
    PIGLIT_REPLAY_LOOP_TIMES \
    PIGLIT_REPLAY_REFERENCE_IMAGES_BASE \
    PIGLIT_REPLAY_SUBCOMMAND \
    PIGLIT_RESULTS \
    PIGLIT_TESTS \
    PIPELINE_ARTIFACTS_BASE \
    SKQP_ASSETS_DIR \
    SKQP_BACKENDS \
    TU_DEBUG \
    VIRGL_HOST_API \
    VK_CPU \
    VK_DRIVER \
    VK_ICD_FILENAMES \
    ; do
  if [ -n "${!var+x}" ]; then
    echo "export $var=${!var@Q}"
  fi
done
