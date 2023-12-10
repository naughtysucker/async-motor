#include "async-motor.h"
#include <cstdio>
#include <iostream>

class test_motor : public async_motor_t
{
public:
private:
    double m_speed = 100;

    std::mutex m_mtx;
    double m_position = 0;
    double m_target_position = 0;
    bool m_paused = false;
    bool m_quit = false;

    std::thread m_thread;
public:

    double get_position_impl() override
    {
        std::lock_guard<std::mutex> lck_grd(m_mtx);
        return m_position;
    }

    void move_to_impl(double pos) override
    {
        std::lock_guard<std::mutex> lck_grd(m_mtx);
        m_paused = false;
        m_target_position = pos;
    }

    void pause_impl() override
    {
        std::lock_guard<std::mutex> lck_grd(m_mtx);
        m_paused = true;
    }

    void run()
    {
        while (!m_quit)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::string progress_str;
            for (int i = 0; i < m_position * 2; i++)
            {
                progress_str += "-";
            }
            printf("        Progress: %s\n", progress_str.c_str());
            if (!m_paused)
            {
                std::lock_guard<std::mutex> lck_grd(m_mtx);
                double distance = (m_target_position - m_position) * 0.8;
                distance = std::min(distance, m_speed * 50 / 1000);
                distance = std::max(distance, -m_speed * 50 / 1000);
                m_position += distance;
            }
        }
    }

    test_motor(std::chrono::milliseconds time1, std::chrono::milliseconds time2, double dis1)
        :
            async_motor_t(time1, time2, dis1)
    {
        m_thread = std::move(std::thread(&test_motor::run, this));
    }

    ~test_motor()
    {
        {
            std::lock_guard<std::mutex> lck_grd(m_mtx);
            m_quit = true;
        }
        m_thread.join();
    }
};

class test_workflow
{
public:
private:
    test_motor tm_ins;
public:
    void run_pause()
    {
        while (1)
        {
            tm_ins.pause();
            double pos_before = tm_ins.get_actual_position();
            printf("paused\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(333));
            double pos_after = tm_ins.get_actual_position();
            if (std::abs(pos_after - pos_before) > 1)
            {
                printf("Pause Failed\n");
                exit(0);
            }
            tm_ins.resume();
            printf("resumed\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(222));
        }
    }
    void run_move()
    {
        while (1)
        {
            std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
            try
            {
                printf("\n\n\n\n\n\n\n\nSync Move Start from: %f\n", tm_ins.get_actual_position());
                tm_ins.move_to_sync(50);
            }
            catch (test_motor::exception_t& e)
            {
                printf("Sync Exception: %d, %s", e.get_error(), e.what());
                exit(0);
            }
            std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
            // printf("Sync spends: %lld ms\n", std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());

            begin = std::chrono::steady_clock::now();
            try
            {
                tm_ins.require_async_move_interface();
                printf("\n\n\n\n\n\n\n\nAsync Move Start from: %f\n", tm_ins.get_actual_position());
                tm_ins.move_to_async(0);
                tm_ins.wait_for_moving_done();
                tm_ins.release_async_move_interface();
            }
            catch (test_motor::exception_t& e)
            {
                printf("Async Exception: %d, %s", e.get_error(), e.what());
                exit(0);
            }
            end = std::chrono::steady_clock::now();
            // printf("Async spends: %lld ms\n", std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
        }
    }

    test_workflow()
        :
            tm_ins(std::chrono::milliseconds(100), std::chrono::milliseconds(1000), 1)
    {
        for (int i = 0; i < 10; i++)
        {
            std::thread(&test_workflow::run_move, this).detach();
        }
        for (int i = 0; i < 1; i++)
        {
            std::thread(&test_workflow::run_pause, this).detach();
        }
    }
};


int main(int argc, char **argv)
{
    test_workflow tw;

    while (1);
    return 0;
}