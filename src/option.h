// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2015 Intel Corporation. All Rights Reserved.

#pragma once

#include "backend.h"
#include "archive.h"
#include "hw-monitor.h"
#include "subdevice.h"

#include <chrono>
#include <memory>
#include <vector>

namespace rsimpl
{
    struct option_range
    {
        float min;
        float max;
        float step;
        float def;
    };

    class option
    {
    public:
        virtual void set(float value) = 0;
        virtual float query() const = 0;
        virtual option_range get_range() const = 0;
        virtual bool is_enabled() const = 0;

        virtual const char* get_description() const = 0;
        virtual const char* get_value_description(float) const { return nullptr; }

        virtual ~option() = default;
    };

    class uvc_pu_option : public option
    {
    public:
        void set(float value) override;

        float query() const override;

        option_range get_range() const override;

        bool is_enabled() const override
        {
            return true;
        }

        uvc_pu_option(uvc_endpoint& ep, rs_option id)
            : _ep(ep), _id(id)
        {
        }

        const char* get_description() const override;
    private:
        uvc_endpoint& _ep;
        rs_option _id;
    };

    template<typename T>
    class uvc_xu_option : public option
    {
    public:
        void set(float value) override
        {
            _ep.invoke_powered(
                [this, value](uvc::uvc_device& dev)
                {
                    T t = static_cast<T>(value);
                    dev.set_xu(_xu, _id, reinterpret_cast<uint8_t*>(&t), sizeof(T));
                });
        }
        float query() const override
        {
            return static_cast<float>(_ep.invoke_powered(
                [this](uvc::uvc_device& dev)
                {
                    T t;
                    dev.get_xu(_xu, _id, reinterpret_cast<uint8_t*>(&t), sizeof(T));
                    return static_cast<float>(t);
                }));
        }
        option_range get_range() const override
        {
            auto uvc_range = _ep.invoke_powered(
                [this](uvc::uvc_device& dev)
                {
                    return dev.get_xu_range(_xu, _id, sizeof(T));
                });
            option_range result;
            result.min = static_cast<float>(uvc_range.min);
            result.max = static_cast<float>(uvc_range.max);
            result.def = static_cast<float>(uvc_range.def);
            result.step = static_cast<float>(uvc_range.step);
            return result;
        }
        bool is_enabled() const override { return true; }

        uvc_xu_option(uvc_endpoint& ep, uvc::extension_unit xu, int id, std::string description)
            : _ep(ep), _xu(xu), _id(id), _desciption(std::move(description))
        {
        }

        const char* get_description() const override
        {
            return _desciption.c_str();
        }

    private:
        uvc_endpoint& _ep;
        uvc::extension_unit _xu;
        int _id;
        std::string _desciption;
    };

    inline std::string hexify(unsigned char n)
    {
        std::string res;

        do
        {
            res += "0123456789ABCDEF"[n % 16];
            n >>= 4;
        } while (n);

        reverse(res.begin(), res.end());

        if (res.size() == 1)
        {
            res.insert(0, "0");
        }

        return res;
    }

    class fw_logger_option : public option
    {
    public:
        fw_logger_option(std::weak_ptr<hw_monitor> hw, uint8_t fw_log_op_code, unsigned grab_rate_in_ms, std::string description)
            : _keep_fw_logger_alive(false),
              _hw(hw), _desciption(std::move(description)),
              _fw_log_op_code(fw_log_op_code),
              _grab_rate_in_ms(grab_rate_in_ms)
        {}

        ~fw_logger_option()
        {
            try{
                if (_keep_fw_logger_alive)
                    stop_fw_logger();
            }
            catch(...)
            {

            }
        }

        void start_fw_logger()
        {
            if (_keep_fw_logger_alive)
                throw wrong_api_call_sequence_exception("FW logger already started");

            _keep_fw_logger_alive = true;
            _fw_logger_thread = std::unique_ptr<std::thread>(new std::thread([this]() {
                const int data_size = 500;
                command cmd(_fw_log_op_code);
                cmd.param1 = data_size;
                while (_keep_fw_logger_alive)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(_grab_rate_in_ms));

                    try
                    {
                        auto strong = _hw.lock();
                        if (strong)
                        {
                            auto data = strong->send(cmd);
                            std::stringstream sstr;
                            sstr << "FW_Log_Data:";
                            for (size_t i = 0; i < data.size(); ++i)
                                sstr << hexify(data[i]) << " ";

                            if (!data.empty())
                                LOG_INFO(sstr.str());
                        }
                    }
                    catch(...)
                    {

                    }
                }
            }));
        }

        void stop_fw_logger()
        {
            if (!_keep_fw_logger_alive)
                throw wrong_api_call_sequence_exception("FW logger not started");

            _keep_fw_logger_alive = false;
            _fw_logger_thread->join();
        }

        void set(float value) override
        {
            if (value >= 1)
            {
                start_fw_logger();
            }
            else
            {
                stop_fw_logger();
            }
        }

        float query() const override
        {
            return int(_keep_fw_logger_alive);
        }

        option_range get_range() const override
        {
            option_range result{0, 1, 1, 0};
            return result;
        }

        bool is_enabled() const override { return true; }

        const char* get_description() const override
        {
            return _desciption.c_str();
        }

    private:
        std::atomic<bool> _keep_fw_logger_alive;
        uint8_t _fw_log_op_code;
        unsigned _grab_rate_in_ms;
        std::weak_ptr<hw_monitor> _hw;
        std::string _desciption;
        std::unique_ptr<std::thread> _fw_logger_thread;

    };

    template<class T, class R, class W, class U>
    class struct_feild_option : public option
    {
    public:
        void set(float value) override
        {
            _struct_interface->set(_field, value);
        }
        float query() const override
        {
            return _struct_interface->get(_field);
        }
        option_range get_range() const override
        {
            return _range;
        }
        bool is_enabled() const override { return true; }

        explicit struct_feild_option(std::shared_ptr<struct_interface<T, R, W>> struct_interface,
                                     U T::* field, option_range range)
            : _struct_interface(struct_interface), _range(range), _field(field)
        {
        }

        const char* get_description() const override
        {
            return nullptr;
        }

    private:
        std::shared_ptr<struct_interface<T, R, W>> _struct_interface;
        option_range _range;
        U T::* _field;
    };

    template<class T, class R, class W, class U>
    std::shared_ptr<struct_feild_option<T, R, W, U>> make_field_option(
        std::shared_ptr<struct_interface<T, R, W>> struct_interface,
        U T::* field, option_range range)
    {
        return std::make_shared<struct_feild_option<T, R, W, U>>
            (struct_interface, field, range);
    }

    class command_transfer_over_xu : public uvc::command_transfer
    {
    public:
        std::vector<uint8_t> send_receive(const std::vector<uint8_t>& data, int, bool require_response) override;

        command_transfer_over_xu(uvc_endpoint& uvc, 
                             uvc::extension_unit xu, uint8_t ctrl)
            : _uvc(uvc), _xu(std::move(xu)), _ctrl(ctrl)
        {}

    private:
        uvc_endpoint&       _uvc;
        uvc::extension_unit _xu;
        uint8_t             _ctrl;
    };
}