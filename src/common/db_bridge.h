// Copyright (c) 2012-2018 The Boolberry developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#pragma once

#include <set>
#include <memory>
#include "misc_language.h"
#include "misc_log_ex.h"
#include "currency_core/currency_format_utils.h"
#include "epee/include/db_helpers.h"

namespace db
{
  typedef uint64_t table_id;
  constexpr bool tx_read_write = false;
  constexpr bool tx_read_only = true;

  class i_db_visitor
  {
  public:
    // visitor callback, would be called for each item in a table
    // return value: false to stop the process, true - to continue
    virtual bool on_visit_db_item(size_t i, const void* key_data, size_t key_size, const void* value_data, size_t value_size) = 0;
  };

  class i_db_write_tx_notification_receiver
  {
  public:
    virtual void on_write_transaction_begin() = 0;
    virtual void on_write_transaction_commit() = 0;
    virtual void on_write_transaction_abort() = 0;
  };

  // interface for database implementation
  class i_db_adapter
  {
  public:
    virtual bool open(const std::string& db_name) = 0;
    virtual bool open_table(const std::string& table_name, table_id &tid) = 0;
    virtual bool clear_table(const table_id tid) = 0;
    virtual size_t get_table_size(const table_id tid) = 0;
    virtual bool close() = 0;

    virtual bool begin_transaction(bool read_only_access = false) = 0;
    virtual bool commit_transaction() = 0;
    virtual void abort_transaction() = 0;

    virtual bool get(const table_id tid, const char* key_data, size_t key_size, std::string& out_buffer) = 0;
    virtual bool set(const table_id tid, const char* key_data, size_t key_size, const char* value_data, size_t value_size) = 0;
    virtual bool erase(const table_id tid, const char* key_data, size_t key_size) = 0;

    virtual bool visit_table(const table_id tid, i_db_visitor* visitor) = 0;
    
    virtual ~i_db_adapter()
    {};
  };

  // POD table keys accessors
  template<class tkey_pod_t>
  const char* tkey_to_pointer(const tkey_pod_t& tkey, size_t& len_out) 
  {
    static_assert(std::is_pod<tkey_pod_t>::value, "pod type expected");
    len_out = sizeof tkey;
    return reinterpret_cast<const char*>(&tkey);
  }

  template<class tkey_pod_t>
  void tkey_from_pointer(tkey_pod_t& tkey_out, const void* pointer, const uint64_t len)
  {
    static_assert(std::is_pod<tkey_pod_t>::value, "pod type expected");
    CHECK_AND_ASSERT_THROW_MES(sizeof(tkey_pod_t) == len, "wrong size");
    CHECK_AND_ASSERT_THROW_MES(pointer != nullptr, "pointer is null");
    tkey_out = *static_cast<tkey_pod_t*>(pointer);
  }

  // std::string table keys accessors
  inline const char* tkey_to_pointer(const std::string& key, size_t& len) 
  {
    len = key.size();
    return key.data();
  }

  inline void tkey_from_pointer(std::string& key_out, const void* pointer, uint64_t len)
  {
    CHECK_AND_ASSERT_THROW_MES(pointer, "pointer is null");
    key_out.assign(reinterpret_cast<const char*>(pointer), static_cast<size_t>(len));
  }


  ////////////////////////////////////////////////////////////
  // db_bridge_base
  ////////////////////////////////////////////////////////////
  class db_bridge_base
  {
  public:
    explicit db_bridge_base(std::shared_ptr<i_db_adapter> adapter_ptr)
      : m_db_adapter_ptr(adapter_ptr)
      , m_db_opened(false)
    {}

    ~db_bridge_base()
    {
      close();
    }

    bool begin_db_transaction(bool read_only_access = false)
    {
      // TODO     !!!
      bool r = m_db_adapter_ptr->begin_transaction(read_only_access);

      {
        CRITICAL_REGION_LOCAL(m_attached_container_receivers_lock);        for (auto c : m_attached_container_receivers)
          c->on_write_transaction_begin();
      }

      return r;
    }

    void commit_db_transaction()
    {
      // TODO    !!!
      bool r = m_db_adapter_ptr->commit_transaction();
      CHECK_AND_ASSERT_THROW_MES(r, "commit_transaction failed");

      {
        CRITICAL_REGION_LOCAL(m_attached_container_receivers_lock);        for (auto c : m_attached_container_receivers)
          c->on_write_transaction_commit();
      }
    }

    void abort_db_transaction()
    {
      // TODO     !!!
      m_db_adapter_ptr->abort_transaction();

      {
        CRITICAL_REGION_LOCAL(m_attached_container_receivers_lock);        for (auto c : m_attached_container_receivers)
          c->on_write_transaction_abort();
      }
    }

    bool is_open() const
    {
      return m_db_opened;
    }

    std::shared_ptr<i_db_adapter> get_adapter() const
    {
      return m_db_adapter_ptr;
    }

    bool open(const std::string& db_name)
    {
      m_db_opened = m_db_adapter_ptr->open(db_name);
      return m_db_opened;
    }

    bool close()
    {
      m_db_opened = false;
      return m_db_adapter_ptr->close();
    }

    bool clear(const table_id tid)
    {
      return m_db_adapter_ptr->clear_table(tid);
    }

    uint64_t size(const table_id tid) const
    {
      return m_db_adapter_ptr->get_table_size(tid);
    }

    template<class tkey_pod_t>
    bool erase(const table_id tid, const tkey_pod_t& tkey)
    {
      size_t key_size = 0;
      const char* key_data = tkey_to_pointer(tkey, key_size);
      return m_db_adapter_ptr->erase(tid, key_data, key_size);
    }

    template<class tkey_pod_t, class t_object>
    bool get_serializable_object(const table_id tid, const tkey_pod_t& tkey, t_object& obj) const
    {
      std::string buffer;
      size_t key_size = 0;
      const char* key_data = tkey_to_pointer(tkey, key_size);

      if (!m_db_adapter_ptr->get(tid, key_data, key_size, buffer))
        return false;

      return currency::t_unserializable_object_from_blob(obj, buffer);
    }

    template<class tkey_pod_t, class t_object>
    bool set_serializable_object(const table_id tid, const tkey_pod_t& tkey, const t_object& obj)
    {
      std::string buffer;
      currency::t_serializable_object_to_blob(obj, buffer);

      size_t key_size = 0;
      const char* key_data = tkey_to_pointer(tkey, key_size);
      return m_db_adapter_ptr->set(tid, key_data, key_size, buffer.data(), buffer.size());
    }

    template<class tkey_pod_t, class t_object_pod_t>
    bool get_pod_object(const table_id tid, const tkey_pod_t& tkey, t_object_pod_t& obj) const
    {
      static_assert(std::is_pod<t_object_pod_t>::value, "POD type expected");

      std::string buffer;
      size_t key_size = 0;
      const char* key_data = tkey_to_pointer(tkey, key_size);

      if (!m_db_adapter_ptr->get(tid, key_data, key_size, buffer))
        return false;

      CHECK_AND_ASSERT_MES(sizeof(t_object_pod_t) == buffer.size(), false,
        "DB returned object with size " << buffer.size() << " bytes, while " << sizeof(t_object_pod_t) << " bytes object is expected");

      obj = *reinterpret_cast<const t_object_pod_t*>(buffer.data());
      return true;
    }

    template<class tkey_pod_t, class t_object_pod_t>
    bool set_pod_object(const table_id tid, const tkey_pod_t& tkey, const t_object_pod_t& obj)
    {
      static_assert(std::is_pod<t_object_pod_t>::value, "POD type expected");

      size_t key_size = 0;
      const char* key_data = tkey_to_pointer(tkey, key_size);

      if (!m_db_adapter_ptr->set(tid, key_data, key_size, reinterpret_cast<const char*>(&obj), sizeof obj))
        return false;

      return true;
    }

    void attach_container_receiver(i_db_write_tx_notification_receiver *receiver)
    {
      CRITICAL_REGION_LOCAL(m_attached_container_receivers_lock);      bool r = m_attached_container_receivers.insert(receiver).second;      CHECK_AND_ASSERT_THROW_MES(r, "failed, container already attached");    }

    void detach_container_receiver(i_db_write_tx_notification_receiver *receiver)
    {
      CRITICAL_REGION_LOCAL(m_attached_container_receivers_lock);      bool r = m_attached_container_receivers.erase(receiver) == 1;      CHECK_AND_ASSERT_THROW_MES(r, "failed, container has never been attached");    }

    protected:
      std::shared_ptr<i_db_adapter> m_db_adapter_ptr;
      bool m_db_opened;

    private:
      epee::critical_section m_attached_container_receivers_lock;
      std::set<i_db_write_tx_notification_receiver*> m_attached_container_receivers;

  }; // db_bridge_base


  class pod_object_value_helper
  {
  public:
    template<class value_t>
    static bool tvalue_from_pointer(const void* p, uint64_t s, value_t& v)
    {
      // TODO
      return true;
    }

    template<class key_t, class value_t>
    static std::shared_ptr<const value_t> get(const table_id tid, db_bridge_base& dbb, const key_t& k)
    {
      // TODO
      return nullptr;
    }

    template<class key_t, class value_t>
    static void set(const table_id tid, db_bridge_base& dbb, const key_t& k, const value_t& v)
    {
      // TODO
    }
  };

  class serializable_object_value_helper
  {
  public:
    template<class value_t>
    static bool tvalue_from_pointer(const void* p, uint64_t s, value_t& v)
    {
      // TODO
      return true;
    }

    template<class key_t, class value_t>
    static std::shared_ptr<const value_t> get(const table_id tid, db_bridge_base& dbb, const key_t& k)
    {
      // TODO
      return nullptr;
    }

    template<class key_t, class value_t>
    static void set(const table_id tid, db_bridge_base& dbb, const key_t& k, const value_t& v)
    {
      // TODO
    }
  };

  template<bool value_type_serializable>
  class value_type_helper_selector;

  template<>
  class value_type_helper_selector<true>: public serializable_object_value_helper
  {};

  template<>
  class value_type_helper_selector<false>: public pod_object_value_helper
  {};


  ////////////////////////////////////////////////////////////
  // key_value_accessor_base
  ////////////////////////////////////////////////////////////
  template<class key_t, class value_t, bool value_type_serializable>
  class key_value_accessor_base : public i_db_write_tx_notification_receiver
  {
  public:
    typedef value_t t_value_type;

    key_value_accessor_base(db_bridge_base& dbb)
      : m_dbb(dbb)
      , m_tid(AUTO_VAL_INIT(m_tid))
      , m_cached_size(0)
      , m_cached_size_is_valid(false)
    {
      m_dbb.attach_container_receiver(this);
    }

    ~key_value_accessor_base()
    {
      m_dbb.dettach_container_receiver(this);
    }

    bool begin_transaction(bool read_only = false)
    {
      return m_dbb.begin_transaction(read_only);
    }

    // interface i_db_write_tx_notification_receiver
    virtual void on_write_transaction_begin() override
    {
      m_exclusive_runner.set_exclusive_mode_for_this_thread();
    }

    // interface i_db_write_tx_notification_receiver
    virtual void on_write_transaction_abort() override
    {
      m_exclusive_runner.clear_exclusive_mode_for_this_thread();
    }

    // interface i_db_write_tx_notification_receiver
    virtual void on_write_transaction_commit() override
    {
      m_exclusive_runner.clear_exclusive_mode_for_this_thread();
    }

    
    void commit_transaction()
    {
      try
      {
        m_dbb.commit_transaction();
      }
      catch (...)
      {
        m_cached_size_is_valid = false;
        throw;
      }
    }

    void abort_transaction()
    {
      m_cached_size_is_valid = false;
      m_dbb.abort_transaction();
    }

    bool init(const std::string& table_name)
    {
      return m_dbb.get_adapter()->open_table(table_name, m_tid);
    }

    template<class callback_t>
    void enumerate_keys(callback_t callback) const 
    {
      // TODO
    }

    template<class callback_t>
    void enumerate_items(callback_t callback)const 
    {
      // TODO
    }

    void set(const key_t& key, const value_t& value)
    {
      m_cached_size_is_valid = false;
      value_type_helper_selector<value_type_serializable>::set(m_tid, m_dbb, key, value);
    }

    std::shared_ptr<const value_t> get(const key_t& key) const
    {
      return value_type_helper_selector<value_type_serializable>::template get<key_t, value_t>(m_tid, m_dbb, key);
    }

    template<class explicit_key_t, class explicit_value_t, class object_value_helper_t>
    void explicit_set(const explicit_key_t& key, const explicit_value_t& value)
    {
      m_cached_size_is_valid = false;
      object_value_helper_t::set(m_tid, m_dbb, key, value);
    }

    template<class explicit_key_t, class explicit_value_t, class object_value_helper_t>
    std::shared_ptr<const explicit_value_t> explicit_get(const explicit_key_t& key)
    {
      return object_value_helper_t::template get<explicit_key_t, explicit_value_t>(m_tid, m_dbb, key);
    }

    uint64_t size() const
    {
      return m_exclusive_runner.run<uint64_t>([this](bool exclusive_mode)
      {
        if (exclusive_mode && m_cached_size_is_valid)
          return m_cached_size;

        uint64_t size = m_dbb.size(m_tid);
        if (exclusive_mode)
        {
          m_cached_size = size;
          m_cached_size_is_valid = true;
        }
        return size;
      });
    }

    uint64_t size_no_cache() const
    {
      return m_dbb.size(m_tid);
    }

    size_t clear()
    {
      m_dbb.clear(m_tid);
      m_exclusive_runner.run_exclusively<bool>([this](){
        m_cached_size_is_valid = false;
        return true;
      });
      return true;
    }

    bool erase_validate(const key_t& k)
    {
      auto res_ptr = this->get(k);
      m_dbb.erase(m_tid, k);
      m_exclusive_runner.run_exclusively<bool>([&](){
        m_cached_size_is_valid = false;
        return true;
      });
      return static_cast<bool>(res_ptr);
    }


    void erase(const key_t& k)
    {
      m_dbb.erase(m_tid, k);
      m_exclusive_runner.run_exclusively<bool>([&](){
        m_cached_size_is_valid = false;
        return true;
      });
    }

    std::shared_ptr<const value_t> operator[] (const key_t& k) const
    {
      auto r = this->get(k);
      CHECK_AND_ASSERT_THROW(r.get(), std::out_of_range("Out of range"));
      return r;
    }

  protected:
    table_id m_tid;
    db_bridge_base& m_dbb;
    epee::misc_utils::exclusive_access_helper m_exclusive_runner;

  private:
    mutable uint64_t m_cached_size;
    mutable bool m_cached_size_is_valid;
  }; // class key_value_accessor_base

    

} // namespace db
