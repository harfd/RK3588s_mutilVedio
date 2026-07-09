#include "frameQueue.h"
using FrameData = std::vector<int>; // 示例帧类型
frameQueue<FrameData> data(30);     // 最多缓存 30 帧
void make_thread()
{
    for (int i = 0; i < 100; i++)
    {
        std::cout << "make data:" << i << std::endl;
        FrameData frame = {(i)};
        data.push(std::move(frame));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    data.shutdown(); // 生产完成后关闭队列
}
void consume_thread()
{
    FrameData frame;
    while (data.wait_and_pop(frame))
    {
        if (frame.size() > 10)
        {
            std::cout << "consume data:" << frame[0] << std::endl;
        }
    }
    std::cout << "线程结束" << std::endl;
}
int main()
{
    std::thread t1(make_thread);
    std::thread t2(consume_thread);
    t1.join();
    t2.join();
    return 0;
}