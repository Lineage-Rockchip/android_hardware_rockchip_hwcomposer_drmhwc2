#include "solid_color.h"

#include <cassert>
#include <iostream>

void test() {
  SolidColor manager;

  const size_t WIDTH = 64;
  const size_t HEIGHT = 64;

  // 创建第一个颜色缓冲区
  auto buffer1 = manager.getOrCreate(0xFF0000FF, WIDTH, HEIGHT);  // 红色
  assert(buffer1->getRefCount() == 1);
  assert(buffer1->getWidth() == WIDTH);
  assert(buffer1->getHeight() == HEIGHT);

  // 验证缓冲区内容
  const uint32_t* data1 = buffer1->getBuffer();
  for (size_t i = 0; i < WIDTH * HEIGHT; ++i) {
    assert(data1[i] == 0xFF0000FF);
  }

  // 创建相同颜色和尺寸的第二个引用
  auto buffer2 = manager.getOrCreate(0xFF0000FF, WIDTH, HEIGHT);
  assert(buffer1->getRefCount() == 2);
  assert(buffer2->getRefCount() == 2);
  assert(buffer1->getBuffer() == buffer2->getBuffer());  // 应该共享同一个缓冲区

  // 创建相同颜色但不同尺寸的缓冲区
  auto buffer2_diff = manager.getOrCreate(0xFF0000FF, WIDTH * 2, HEIGHT);
  assert(buffer2_diff->getRefCount() == 1);  // 应该是新的缓冲区
  assert(buffer2_diff->getWidth() == WIDTH * 2);

  // 释放第一个引用
  manager.release(0xFF0000FF, WIDTH, HEIGHT);
  assert(buffer2->getRefCount() == 1);

  // 释放第二个引用
  manager.release(0xFF0000FF, WIDTH, HEIGHT);
  assert(manager.size() == 1);  // 只剩下不同尺寸的缓冲区

  // 释放不同尺寸的缓冲区
  manager.release(0xFF0000FF, WIDTH * 2, HEIGHT);
  assert(manager.size() == 0);

  // 创建不同颜色的缓冲区
  auto buffer3 = manager.getOrCreate(0x00FF00FF, WIDTH, HEIGHT);  // 绿色
  assert(manager.size() == 1);
  assert(buffer3->getRefCount() == 1);

  // 验证新缓冲区内容
  const uint32_t* data3 = buffer3->getBuffer();
  for (size_t i = 0; i < WIDTH * HEIGHT; ++i) {
    assert(data3[i] == 0x00FF00FF);
  }
}

int main() {
  test();
  std::cout << "All tests passed!" << std::endl;
  return 0;
}