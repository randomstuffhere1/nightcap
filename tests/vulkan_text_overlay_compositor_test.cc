#include "mocktail/graphics/vulkan_text_overlay_compositor.h"

#include <gtest/gtest.h>
#include <libplacebo/vulkan.h>

#include <atomic>
#include <cstdint>

namespace mocktail {
namespace graphics {
namespace {

std::atomic<int> g_submit_calls{0};
std::atomic<int> g_submit2_calls{0};
std::atomic<int> g_bind_sparse_calls{0};
std::atomic<int> g_wait_idle_calls{0};
std::atomic<VkQueue> g_last_queue{VK_NULL_HANDLE};

VkQueue FakeQueue() {
  return reinterpret_cast<VkQueue>(static_cast<uintptr_t>(0x51b));
}

VkResult RecordingQueueSubmit(VkQueue queue, uint32_t, const VkSubmitInfo*,
                              VkFence) {
  g_submit_calls.fetch_add(1, std::memory_order_relaxed);
  g_last_queue.store(queue, std::memory_order_relaxed);
  return VK_SUCCESS;
}

VkResult RecordingQueueSubmit2(VkQueue queue, uint32_t, const VkSubmitInfo2*,
                               VkFence) {
  g_submit2_calls.fetch_add(1, std::memory_order_relaxed);
  g_last_queue.store(queue, std::memory_order_relaxed);
  return VK_SUCCESS;
}

VkResult RecordingQueueBindSparse(VkQueue queue, uint32_t,
                                  const VkBindSparseInfo*, VkFence) {
  g_bind_sparse_calls.fetch_add(1, std::memory_order_relaxed);
  g_last_queue.store(queue, std::memory_order_relaxed);
  return VK_SUCCESS;
}

VkResult RecordingQueueWaitIdle(VkQueue queue) {
  g_wait_idle_calls.fetch_add(1, std::memory_order_relaxed);
  g_last_queue.store(queue, std::memory_order_relaxed);
  return VK_SUCCESS;
}

class VulkanTextOverlayCompositorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_submit_calls.store(0, std::memory_order_relaxed);
    g_submit2_calls.store(0, std::memory_order_relaxed);
    g_bind_sparse_calls.store(0, std::memory_order_relaxed);
    g_wait_idle_calls.store(0, std::memory_order_relaxed);
    g_last_queue.store(VK_NULL_HANDLE, std::memory_order_relaxed);
  }
};

class VulkanTextOverlayCompositorDeviceTest
    : public VulkanTextOverlayCompositorTest {
 protected:
  void SetUp() override {
    VulkanTextOverlayCompositorTest::SetUp();
    pl_vulkan_params params = pl_vulkan_default_params;
    params.allow_software = true;
    params.async_compute = false;
    params.async_transfer = false;
    host_ = pl_vulkan_create(nullptr, &params);
    if (host_ == nullptr) {
      GTEST_SKIP() << "A compatible Vulkan device is unavailable";
    }
    get_device_proc_addr_ = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        host_->get_proc_addr(host_->instance, "vkGetDeviceProcAddr"));
    ASSERT_NE(get_device_proc_addr_, nullptr);
  }

  void TearDown() override { pl_vulkan_destroy(&host_); }

  bool RegisterHostDevice(VulkanTextOverlayCompositor* compositor) {
    return compositor->RegisterDevice(
        host_->instance, host_->phys_device, host_->device, host_->api_version,
        host_->extensions, host_->num_extensions, host_->queue_graphics.index,
        host_->queue_graphics.count, host_->features, host_->get_proc_addr,
        get_device_proc_addr_);
  }

  pl_vulkan host_ = nullptr;
  PFN_vkGetDeviceProcAddr get_device_proc_addr_ = nullptr;
};

TEST_F(VulkanTextOverlayCompositorDeviceTest,
       RegisteredDeviceTeardownRemovesStateAndPreservesHost) {
  VulkanTextOverlayCompositor compositor;
  ASSERT_TRUE(RegisterHostDevice(&compositor));
  const auto get_device_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(
      get_device_proc_addr_(host_->device, "vkGetDeviceQueue"));
  ASSERT_NE(get_device_queue, nullptr);
  VkQueue queue = VK_NULL_HANDLE;
  get_device_queue(host_->device, host_->queue_graphics.index, 0, &queue);
  ASSERT_NE(queue, VK_NULL_HANDLE);
  ASSERT_TRUE(compositor.RegisterQueue(host_->device, queue,
                                       host_->queue_graphics.index, 0));

  compositor.DestroyDevice(VK_NULL_HANDLE);
  compositor.DestroyDevice(
      reinterpret_cast<VkDevice>(static_cast<uintptr_t>(0x51d)));
  ASSERT_TRUE(compositor.RegisterQueue(host_->device, queue,
                                       host_->queue_graphics.index, 0));

  // Run with ThreadSanitizer to detect an unlock after DeviceState is freed.
  compositor.DestroyDevice(host_->device);
  EXPECT_FALSE(compositor.RegisterQueue(host_->device, queue,
                                        host_->queue_graphics.index, 0));
  compositor.DestroyDevice(host_->device);
  EXPECT_EQ(compositor.QueueSubmit(queue, 0, nullptr, VK_NULL_HANDLE,
                                   RecordingQueueSubmit),
            VK_SUCCESS);
  EXPECT_EQ(g_submit_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_last_queue.load(std::memory_order_relaxed), queue);

  const auto device_wait_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(
      get_device_proc_addr_(host_->device, "vkDeviceWaitIdle"));
  ASSERT_NE(device_wait_idle, nullptr);
  EXPECT_EQ(device_wait_idle(host_->device), VK_SUCCESS);
  ASSERT_TRUE(RegisterHostDevice(&compositor));
  compositor.DestroyDevice(host_->device);
}

// The fullscreen flash fix: RegisterSwapchain clears every image it is handed,
// so a frame presented before the guest has rebuilt its render targets at the
// new extent shows black rather than uninitialised driver contents.
//
// There is no presentation surface in a test, so this registers a swapchain
// handle that does not exist -- the clear path only uses the VkImage values it
// is given, and the images here are real host allocations. What is asserted is
// that registration succeeds and the submission lands on the graphics queue,
// which is the property the fix depends on. The contents are not read back:
// reading them requires a transition that a swapchain image is not required to
// support, and the point of the test is the submission, not the pixels.
TEST_F(VulkanTextOverlayCompositorDeviceTest,
       RegisterSwapchainClearsImagesWithoutBreakingRegistration) {
  VulkanTextOverlayCompositor compositor;
  ASSERT_TRUE(RegisterHostDevice(&compositor));

  constexpr std::uint32_t kWidth = 64;
  constexpr std::uint32_t kHeight = 32;
  constexpr std::uint32_t kImageCount = 2;

  auto* create_image = reinterpret_cast<PFN_vkCreateImage>(
      get_device_proc_addr_(host_->device, "vkCreateImage"));
  auto* destroy_image = reinterpret_cast<PFN_vkDestroyImage>(
      get_device_proc_addr_(host_->device, "vkDestroyImage"));
  auto* get_memory_requirements = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
      get_device_proc_addr_(host_->device, "vkGetImageMemoryRequirements"));
  auto* allocate_memory = reinterpret_cast<PFN_vkAllocateMemory>(
      get_device_proc_addr_(host_->device, "vkAllocateMemory"));
  auto* free_memory = reinterpret_cast<PFN_vkFreeMemory>(
      get_device_proc_addr_(host_->device, "vkFreeMemory"));
  auto* bind_image = reinterpret_cast<PFN_vkBindImageMemory>(
      get_device_proc_addr_(host_->device, "vkBindImageMemory"));
  ASSERT_NE(create_image, nullptr);
  ASSERT_NE(destroy_image, nullptr);
  ASSERT_NE(get_memory_requirements, nullptr);
  ASSERT_NE(allocate_memory, nullptr);
  ASSERT_NE(free_memory, nullptr);
  ASSERT_NE(bind_image, nullptr);

  VkImage images[kImageCount] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkDeviceMemory memory[kImageCount] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  for (std::uint32_t index = 0; index < kImageCount; ++index) {
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {kWidth, kHeight, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ASSERT_EQ(create_image(host_->device, &image_info, nullptr, &images[index]),
              VK_SUCCESS);

    VkMemoryRequirements requirements{};
    get_memory_requirements(host_->device, images[index], &requirements);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = 0;
    // Memory type 0 is valid for any host that can create an image; if the
    // device reports otherwise the clear path is untestable here.
    if (allocate_memory(host_->device, &allocate, nullptr, &memory[index]) !=
        VK_SUCCESS) {
      for (std::uint32_t cleanup = 0; cleanup <= index; ++cleanup) {
        if (memory[cleanup] != VK_NULL_HANDLE) {
          free_memory(host_->device, memory[cleanup], nullptr);
        }
        if (images[cleanup] != VK_NULL_HANDLE) {
          destroy_image(host_->device, images[cleanup], nullptr);
        }
      }
      GTEST_SKIP() << "no allocatable memory type for the test image";
    }
    ASSERT_EQ(bind_image(host_->device, images[index], memory[index], 0),
              VK_SUCCESS);
  }

  VkSurfaceKHR surface = reinterpret_cast<VkSurfaceKHR>(
      static_cast<uintptr_t>(0x51de1));
  VkSwapchainCreateInfoKHR create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  create_info.surface = surface;
  create_info.minImageCount = kImageCount;
  create_info.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
  create_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  create_info.imageExtent = {kWidth, kHeight};
  create_info.imageArrayLayers = 1;
  create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  create_info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  create_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  create_info.clipped = VK_TRUE;

  const SwapchainClearStats before =
      TextOverlaySwapchainClearStats();

  // Registration must still succeed: the clear is best effort and must never
  // turn a successful swapchain creation into a failure the guest cannot
  // recover from.
  EXPECT_TRUE(compositor.RegisterSwapchain(host_->device,
                                           reinterpret_cast<VkSwapchainKHR>(
                                               static_cast<uintptr_t>(0x51de)),
                                           create_info, images, kImageCount));

  // The clear actually ran, against the real device and the real graphics
  // queue. Asserted through the submission counter because the clear uses the
  // device's own vkQueueSubmit, which the RecordingQueueSubmit fallback seam
  // cannot intercept.
  const SwapchainClearStats after_first =
      TextOverlaySwapchainClearStats();
  EXPECT_EQ(after_first.images_cleared - before.images_cleared, kImageCount)
      << "RegisterSwapchain did not clear every image it was given";
  EXPECT_EQ(after_first.submit_failures, before.submit_failures)
      << "the swapchain image clear failed to submit";

  // Registering the same swapchain again is idempotent and must not clear twice.
  EXPECT_TRUE(compositor.RegisterSwapchain(host_->device,
                                           reinterpret_cast<VkSwapchainKHR>(
                                               static_cast<uintptr_t>(0x51de)),
                                           create_info, images, kImageCount));
  EXPECT_EQ(TextOverlaySwapchainClearStats().images_cleared,
            after_first.images_cleared)
      << "re-registering an existing swapchain cleared its images again";

  compositor.DestroySwapchain(host_->device,
                              reinterpret_cast<VkSwapchainKHR>(
                                  static_cast<uintptr_t>(0x51de)));
  compositor.DestroyDevice(host_->device);

  for (std::uint32_t index = 0; index < kImageCount; ++index) {
    free_memory(host_->device, memory[index], nullptr);
    destroy_image(host_->device, images[index], nullptr);
  }
}

TEST_F(VulkanTextOverlayCompositorTest,
       InactiveOverlayQueueSubmitInvokesFallback) {
  VulkanTextOverlayCompositor compositor;
  const VkQueue queue = FakeQueue();
  EXPECT_EQ(compositor.QueueSubmit(queue, 0, nullptr, VK_NULL_HANDLE,
                                   RecordingQueueSubmit),
            VK_SUCCESS);
  EXPECT_EQ(g_submit_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_last_queue.load(std::memory_order_relaxed), queue);
}

TEST_F(VulkanTextOverlayCompositorTest,
       InactiveOverlayQueueSubmit2WaitIdleAndBindSparseInvokeFallback) {
  VulkanTextOverlayCompositor compositor;
  const VkQueue queue = FakeQueue();
  EXPECT_EQ(compositor.QueueSubmit2(queue, 0, nullptr, VK_NULL_HANDLE,
                                    RecordingQueueSubmit2),
            VK_SUCCESS);
  EXPECT_EQ(compositor.QueueBindSparse(queue, 0, nullptr, VK_NULL_HANDLE,
                                       RecordingQueueBindSparse),
            VK_SUCCESS);
  EXPECT_EQ(compositor.QueueWaitIdle(queue, RecordingQueueWaitIdle),
            VK_SUCCESS);
  EXPECT_EQ(g_submit2_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_bind_sparse_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_wait_idle_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_last_queue.load(std::memory_order_relaxed), queue);
}

TEST_F(VulkanTextOverlayCompositorTest, NullFallbackFailsClosed) {
  VulkanTextOverlayCompositor compositor;
  const VkQueue queue = FakeQueue();
  EXPECT_EQ(
      compositor.QueueSubmit(queue, 0, nullptr, VK_NULL_HANDLE, nullptr),
      VK_ERROR_INITIALIZATION_FAILED);
  EXPECT_EQ(
      compositor.QueueSubmit2(queue, 0, nullptr, VK_NULL_HANDLE, nullptr),
      VK_ERROR_INITIALIZATION_FAILED);
  EXPECT_EQ(
      compositor.QueueBindSparse(queue, 0, nullptr, VK_NULL_HANDLE, nullptr),
      VK_ERROR_INITIALIZATION_FAILED);
  EXPECT_EQ(compositor.QueueWaitIdle(queue, nullptr),
            VK_ERROR_INITIALIZATION_FAILED);
  EXPECT_EQ(g_submit_calls.load(std::memory_order_relaxed), 0);
}

}  // namespace
}  // namespace graphics
}  // namespace mocktail
