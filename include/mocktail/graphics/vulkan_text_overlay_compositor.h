#ifndef MOCKTAIL_GRAPHICS_VULKAN_TEXT_OVERLAY_COMPOSITOR_H_
#define MOCKTAIL_GRAPHICS_VULKAN_TEXT_OVERLAY_COMPOSITOR_H_

#include <vulkan/vulkan.h>

#include <cstdint>

namespace mocktail {
namespace graphics {

// How many swapchain images have been cleared to opaque black, and how many
// clear attempts failed to submit.
//
// A swapchain image's contents are undefined until something writes them, so a
// frame presented before the guest has rebuilt its render targets at a new
// extent shows driver contents rather than an image. That was the brief pink
// and green flash on fullscreen. RegisterSwapchain clears each image once at
// creation to remove the class of artefact; these counters are how a test
// observes that the clear ran, and how the field can tell whether it is
// working on a given driver.
struct SwapchainClearStats {
  std::uint64_t images_cleared = 0;
  std::uint64_t submit_failures = 0;
};

// Registered Vulkan objects remain caller-owned and must be unregistered
// before host destruction. Unsupported registrations fall back cleanly.
class VulkanTextOverlayCompositor final {
 public:
  VulkanTextOverlayCompositor();
  ~VulkanTextOverlayCompositor();

  VulkanTextOverlayCompositor(const VulkanTextOverlayCompositor&) = delete;
  VulkanTextOverlayCompositor& operator=(const VulkanTextOverlayCompositor&) =
      delete;

  // enabled_features must match vkCreateDevice. Proc callbacks resolve host
  // functions directly, bypassing the Android adapter.
  bool RegisterDevice(VkInstance instance, VkPhysicalDevice physical_device,
                      VkDevice device, std::uint32_t api_version,
                      const char* const* enabled_extensions,
                      std::uint32_t enabled_extension_count,
                      std::uint32_t graphics_queue_family,
                      std::uint32_t graphics_queue_count,
                      const VkPhysicalDeviceFeatures2* enabled_features,
                      PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                      PFN_vkGetDeviceProcAddr get_device_proc_addr);
  void DestroyDevice(VkDevice device);

  bool RegisterQueue(VkDevice device, VkQueue queue, std::uint32_t queue_family,
                     std::uint32_t queue_index);

  bool RegisterSwapchain(VkDevice device, VkSwapchainKHR swapchain,
                         const VkSwapchainCreateInfoKHR& create_info,
                         const VkImage* images, std::uint32_t image_count);
  void DestroySwapchain(VkDevice device, VkSwapchainKHR swapchain);

  // Adapter operations serialize only queues imported by libplacebo.
  VkResult QueueSubmit(VkQueue queue, std::uint32_t submit_count,
                       const VkSubmitInfo* submits, VkFence fence,
                       PFN_vkQueueSubmit fallback);
  VkResult QueueSubmit2(VkQueue queue, std::uint32_t submit_count,
                        const VkSubmitInfo2* submits, VkFence fence,
                        PFN_vkQueueSubmit2 fallback);
  VkResult QueueBindSparse(VkQueue queue, std::uint32_t bind_info_count,
                           const VkBindSparseInfo* bind_info, VkFence fence,
                           PFN_vkQueueBindSparse fallback);
  VkResult QueueWaitIdle(VkQueue queue, PFN_vkQueueWaitIdle fallback);
  VkResult DeviceWaitIdle(VkDevice device, PFN_vkDeviceWaitIdle fallback);

  // After wait collapse, fallback receives only compositor-owned semaphores.
  VkResult QueuePresent(VkQueue queue, const VkPresentInfoKHR* present_info,
                        PFN_vkQueuePresentKHR fallback);

  // Aggregate of the swapchain image clear counters across every live
  // compositor. Static because the counters live in the private Impl.
  static void AccumulateSwapchainClearStats(SwapchainClearStats* out);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

SwapchainClearStats TextOverlaySwapchainClearStats();



}  // namespace graphics
}  // namespace mocktail

#endif  // MOCKTAIL_GRAPHICS_VULKAN_TEXT_OVERLAY_COMPOSITOR_H_
