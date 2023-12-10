#pragma once
#include <thread>
#include <mutex>
#include <condition_variable>
#include <exception>
#include <chrono>

class async_motor_t
{
public:
    enum class status_t
    {
        idle,
        moving,
        move_ok,
        error,
    };

    enum class command_t
    {
        none,
        move,
        pause,
        resume,
        quit,
    };

    class exception_t : public std::exception
    {
    public:
        enum error_t
        {
            move_timeout,
            interface_sync_error,
            status_error,
            unknown_error,
        };
    private:
        error_t m_error;
        std::string m_string;
    public:
        exception_t(error_t err)
        {
            if (err == move_timeout)
            {
                m_string = "Move Timeout";
            }
            if (err == interface_sync_error)
            {
                m_string = "Interface Sync Error";
            }
            if (err == status_error)
            {
                m_string = "Status Error";
            }
            if (err == unknown_error)
            {
                m_string = "Unknown Error";
            }
        }

        error_t get_error()
        {
            return m_error;
        }

        const char *what() const noexcept override
        {
            return m_string.c_str();
        }
    };

private:

    std::thread m_thread;

    std::mutex m_mtx_interface_sync;

    std::mutex m_mtx_command;
    std::condition_variable m_cv_command;
    command_t m_command = command_t::none;
    double m_command_param = 0;

    std::mutex m_mtx_status;
    std::condition_variable m_cv_status;
    status_t m_status = status_t::idle;
    exception_t m_error = exception_t::unknown_error;

    std::mutex m_mtx_paused;
    std::condition_variable m_cv_paused;
    bool m_paused = false;

    std::mutex m_mtx_target_position;
    double m_target_position = 0;
    std::mutex m_mtx_actual_position;
    double m_actual_position = 0;

    double m_distance_threshold = 0;

    std::chrono::steady_clock::time_point m_command_begin_time_point;
    std::chrono::milliseconds m_timeout_threshold;

    std::chrono::milliseconds m_loop_wait_time;

    void run()
    {
        while (1)
        {
            command_t command = command_t::none;
            double command_param = 0;

            {
                std::unique_lock<std::mutex> uni_lck(m_mtx_command);
                m_cv_command.wait_for(uni_lck, m_loop_wait_time, [=](){return (m_command != command_t::none || [=](){ std::lock_guard<std::mutex> lck_grd_status(m_mtx_status); return m_status; }() == status_t::moving) && !([=](){std::lock_guard<std::mutex> lck_grd_paused(m_mtx_paused); return m_paused;}() && m_command != command_t::resume);});
                command = m_command;
                command_param = m_command_param;
                m_command = command_t::none;
            }

            if (command == command_t::move)
            {
                std::lock_guard<std::mutex> lck_grd(m_mtx_target_position);
                m_target_position = command_param;
                bool paused = false;
                {
                    std::lock_guard<std::mutex> lck_grd(m_mtx_paused);
                    paused = m_paused;
                }
                if (!paused)
                {
                    m_command_begin_time_point = std::chrono::steady_clock::now();
                    move_to_impl(m_target_position);
                }
                std::lock_guard<std::mutex> lck_grd_status(m_mtx_status);
                m_status = status_t::moving;
                m_cv_status.notify_all();
            }
            else if (command == command_t::pause)
            {
                pause_impl();
                std::lock_guard<std::mutex> lck_grd(m_mtx_paused);
                m_paused = true;
                m_cv_paused.notify_all();
            }
            else if (command == command_t::resume)
            {
                m_command_begin_time_point = std::chrono::steady_clock::now();
                move_to_impl(m_command_param);
                std::lock_guard<std::mutex> lck_grd(m_mtx_paused);
                m_paused = false;
                m_cv_paused.notify_all();
            }
            else if (command == command_t::quit)
            {
                break;
            }

            double actual_position = get_position_impl();
            double target_position = 0;
            {
                std::lock_guard<std::mutex> lck_grd(m_mtx_actual_position);
                m_actual_position = actual_position;
            }
            {
                std::lock_guard<std::mutex> lck_grd(m_mtx_target_position);
                target_position = m_target_position;
            }
            {
                std::lock_guard<std::mutex> lck_grd_status(m_mtx_status);
                std::lock_guard<std::mutex> lck_grd_paused(m_mtx_paused);
                if (m_status == status_t::moving && !m_paused)
                {
                    if (std::abs(m_target_position - actual_position) < 1)
                    {
                        m_status = status_t::move_ok;
                        m_cv_status.notify_all();
                    }
                    else
                    {
                        std::chrono::steady_clock::time_point now_time_point = std::chrono::steady_clock::now();
                        std::chrono::milliseconds duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now_time_point - m_command_begin_time_point);
                        if (duration_ms > m_timeout_threshold)
                        {
                            m_status = status_t::error;
                            m_cv_status.notify_all();
                            m_error = exception_t::move_timeout;
                        }
                    }
                }
            }
        }
    }

    void quit() noexcept
    {
        std::unique_lock<std::mutex> uni_lck(m_mtx_command);
        m_command = command_t::quit;
        m_cv_command.notify_all();
    }

    void check_error_and_throw()
    {
        std::unique_lock<std::mutex> uni_lck(m_mtx_status);
        if (m_status == status_t::error)
        {
            throw m_error;
        }
    }

    void check_not_idle_and_throw()
    {
        std::unique_lock<std::mutex> uni_lck(m_mtx_status);
        if (m_status != status_t::idle)
        {
            throw exception_t(exception_t::interface_sync_error);
        }
    }

public:

    async_motor_t(std::chrono::milliseconds time_loop_wait, std::chrono::milliseconds timeout_moving, double distance_threshold)
        :
            m_loop_wait_time(time_loop_wait),
            m_timeout_threshold(timeout_moving),
            m_distance_threshold(distance_threshold)
    {
        m_thread = std::move(std::thread(&async_motor_t::run, this));
    }

    ~async_motor_t()
    {
        quit();
        m_thread.join();
    }

#if 0
    std::mutex& get_interface_sync_mtx()
    {
        return m_mtx_interface_sync;
    }
#endif

    // implement
    virtual double get_position_impl() = 0;

    // implement
    virtual void move_to_impl(double pos) = 0;

    // implement
    virtual void pause_impl() = 0;

    // interface
    void move_to_async(double target_pos)
    {
        check_error_and_throw();
        check_not_idle_and_throw();
        {
            std::unique_lock<std::mutex> uni_lck(m_mtx_command);
            m_cv_command.wait(uni_lck, [=](){return m_command == command_t::none;});
            m_command = command_t::move;
            m_command_param = target_pos;
            m_cv_command.notify_all();
        }
    }

    // interface
    void wait_for_moving_done()
    {
        std::unique_lock<std::mutex> uni_lck(m_mtx_status);
        m_cv_status.wait(uni_lck, [=](){return m_status != status_t::moving && m_status != status_t::idle;});
        if (m_status != status_t::move_ok)
        {
            if (m_status == status_t::error)
            {
                throw m_error;
            }
            else
            {
                throw exception_t(exception_t::unknown_error);
            }
        }
        m_status = status_t::idle;
        m_cv_status.notify_all();
    }

    // interface
    void move_to_sync(double target_pos)
    {
        std::lock_guard<std::mutex> lck_grd(m_mtx_interface_sync);
        move_to_async(target_pos);
        wait_for_moving_done();
    }

    // interface
    void move_distance_async(double distance)
    {
        double current_position = get_position_impl();
        move_to_async(current_position + distance);
    }

    // interface
    void move_distance_sync(double distance)
    {
        std::lock_guard<std::mutex> lck_grd(m_mtx_interface_sync);
        double current_position = get_position_impl();
        move_to_async(current_position + distance);
        wait_for_moving_done();
    }

    // interface
    void pause()
    {
        check_error_and_throw();
        {
            std::unique_lock<std::mutex> uni_lck(m_mtx_command);
            m_cv_command.wait(uni_lck, [=](){return m_command == command_t::none;});
            m_command = command_t::pause;
            m_cv_command.notify_all();
        }
        {
            std::unique_lock<std::mutex> uni_lck(m_mtx_paused);
            m_cv_paused.wait(uni_lck, [=](){return m_paused;});
        }
    }

    // interface
    void resume()
    {
        check_error_and_throw();
        {
            std::unique_lock<std::mutex> uni_lck(m_mtx_command);
            m_cv_command.wait(uni_lck, [=](){return m_command == command_t::none;});
            m_command = command_t::resume;
            m_cv_command.notify_all();
        }
        {
            std::unique_lock<std::mutex> uni_lck(m_mtx_paused);
            m_cv_paused.wait(uni_lck, [=](){return !m_paused;});
        }
    }

    // interface
    double get_actual_position()
    {
        std::lock_guard<std::mutex> lck_grd(m_mtx_actual_position);
        return m_actual_position;
    }

    // interface
    double get_target_position()
    {
        std::lock_guard<std::mutex> lck_grd(m_mtx_target_position);
        return m_target_position;
    }

    // interface
    void require_async_move_interface()
    {
        m_mtx_interface_sync.lock();
    }

    // interface
    void release_async_move_interface()
    {
        std::lock_guard<std::mutex> lck_grd(m_mtx_status);
        m_status = status_t::idle;
        m_cv_status.notify_all();
        m_mtx_interface_sync.unlock();
    }

};