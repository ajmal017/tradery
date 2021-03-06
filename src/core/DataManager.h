/*
Copyright (C) 2018 Adrian Michel
http://www.amichel.com

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

template <class T>
class DataCacheable : public Cacheable<T> {
 private:
  const std::string _stamp;

 public:
  DataCacheable(T* t, const Id& id, const std::string& stamp)
      : Cacheable<T>(t, id), _stamp(stamp) {}

  const std::string& stamp() const { return _stamp; }
};

/**
 * Interface (abstract base class) - is an intermediate layer between system
 * code and data sources. It provides functionality such as caching and others
 *
 * Currently, the only data manager available is implemented internally, so the
 * user is not supposed to derive concrete classes from this one, but instead
 * will use the "create" method to crate instances of the exiting class.
 *
 * //TODO: more detailed description
 *
 * @see DataSource
 */
class DataManager : public DataRequester {
 public:
  /**
   * Create an instance of the internally implemented DataManagerI class
   *
   * The pointer is owned and needs to be deleted by the application
   *
   * @return Pointer to a newly created DataManager
   */
  static DataManager* create(unsigned int cacheSize);
  virtual ~DataManager() {}
  /**
   * Adds a new data source to the DataManager.
   *
   * The pointer to the data source is onwed and must be deleted by the
   * application
   *
   * @param dataSource
   *               pointer to a DataSource object
   */
  virtual void addDataSource(const DataSource* dataSource) = 0;
  virtual bool removeDataSource(const DataSource* dataSource) = 0;
  virtual bool removeDataSource(const UniqueId& dataSourceId) = 0;
  /**
   * Enables/disables caching
   *
   * @param enable enable caching if true, disable caching if false
   */
  virtual void enableCaching(bool enable) = 0;
  virtual void setCacheSize(unsigned int size) = 0;
};

/**
 * Data Manager is a data server class: it sits between the simulator
 * and the data, which could come from anywhere.
 * Data manager could contain one or more caches, or it can
 * be connected to a database, or can ask the appropriate
 * module to calculate and construct the data objects.
 */

template <class T>
class XCounter {
 private:
  unsigned long _count;
  T _t;

 public:
  XCounter(T t) : _count(1), _t(t) {}

  T get() { return _t; }
  unsigned long increment() { return ++_count; }
  bool decrement() { return --_count == 0; }
  unsigned long getCount() const { return _count; }
};

typedef XCounter<const DataSource*> DataSourceCounter;
typedef ManagedPtr<DataSourceCounter> DataSourceCounterPtr;

typedef std::map<UniqueId, DataSourceCounterPtr> DSMap;
class DataManagerImpl : public DataManager {
 private:
  DataCache _cache;

  DSMap _dataSources;

 public:
  DataManagerImpl::DataManagerImpl(unsigned int cacheSize)
      : _cache(cacheSize, true) {}

  virtual DataManagerImpl::~DataManagerImpl() {
    // make sure we have unregistered as many as we have registered
    assert(_dataSources.size() == 0);
  }

  // this is necessary because we need to do find and add to cache in one move.
  // otherwise, it could happen that two threads call the same method at the
  // same time and they both can't find the bars in the cache, and then both add
  // it, which could create problems.
  Mutex _mutex;

 private:
  bool removeDataSource(const UniqueId& id) {
    DSMap::iterator i = _dataSources.find(id);

    if (i != _dataSources.end()) {
      LOG(log_info,
          "removing datasource: " << i->first.toString()
                                  << ", count: " << i->second->getCount());
      if (i->second->decrement()) {
        LOG(log_info, "removing datasource from data manager");
        _dataSources.erase(i);
        return true;
      } else {
        return false;
      }
    } else {
      LOG(log_error, "can't remove data source that hasn't been registered: "
                         << i->first.toString());
      throw std::exception(
          (std::string("DataManager - trying to remove data source that has "
                       "not been registered: ") +
           id.toString())
              .c_str());
    }
  }

 public:
  void addDataSource(const DataSource* dataSource) throw(
      DataSourceAlreadyRegisteredException) {
    assert(dataSource != 0);
    LOG(log_info, "adding datasource: " << dataSource->id().toString());

    tradery::Lock lock(_mutex);
    DSMap::iterator i = _dataSources.find(dataSource->id());
    if (i == _dataSources.end()) {
      LOG(log_info, "new datasource: " << dataSource->id().toString());
      _dataSources.insert(DSMap::value_type(
          dataSource->id(),
          DataSourceCounterPtr(new DataSourceCounter(dataSource))));
    } else {
      LOG(log_info, "datasource already present, current count: "
                        << i->second->getCount() << ", incrementing");
      unsigned long count = i->second->increment();
    }
  }

  bool removeDataSource(const DataSource* dataSource) {
    assert(dataSource != 0);
    tradery::Lock lock(_mutex);
    return removeDataSource(dataSource->id());
  }

  void enableCaching(bool enable) { _cache.enable(enable); }

  template <class T>
  class MakeDataX : public CacheableBuilder<T> {
   private:
    const DataInfo* _dataInfo;
    const DataSource* _dataSource;
    DateTimeRangePtr _range;
    const Id _id;

   private:
    static Id calculateId(const DataInfo* dataInfo, DateTimeRangePtr range) {
      Id id = dataInfo->id();
      if (range) id += range->getId();
      return id;
    }

   public:
    MakeDataX(const DataInfo* dataInfo, const DataSource* dataSource,
              DateTimeRangePtr range)
        : _dataInfo(dataInfo),
          _dataSource(dataSource),
          _range(range),
          _id(calculateId(dataInfo, range)) {}

    virtual boost::shared_ptr<Cacheable<T> > make() const {
      // put this temporarily into a auto_ptr just in case there is an exception
      const DataSource::DataXPtr b(_dataSource->getData(_dataInfo, _range));
      if (b->getSize() == 0)
        // TODO: add range to the exception
        throw DataSourceException(
            DATA_ERROR,
            std::string(
                "No data available in the requested range for symbol: \"") +
                _dataInfo->symbol().symbol() + "\"",
            _dataSource->name());
      else
        // release the pointer in the auto_ptr, so it won't delete it
        return boost::shared_ptr<Cacheable<T> >(new DataCacheable<T>(
            b->releaseDataCollection(), id(), b->getStamp()));
    }

    virtual const Id& id() const { return _id; }

    virtual bool isConsistent(const Cacheable<T>& cacheable) const {
      try {
        const DataCacheable<T>& dc =
            dynamic_cast<const DataCacheable<T>&>(cacheable);
        return _dataSource->isConsistent(dc.stamp(), _dataInfo->symbol(),
                                         _range);
      } catch (std::bad_cast e) {
        assert(true);
        return false;
      }
    }
  };

  typedef MakeDataX<const DataCollection> MakeData;

  // TODO: this is just a placeholder method - it returns a Bars object given
  // some string (I don't know what the string is yet)
  // change it so it returns a better type and it takes the right argument
  DataManagedPtr getData(const DataInfo* di,
                         DateTimeRangePtr range) throw(DataSourceException) {
    /*    Lock lock( _mutex );

        //TODO: the id is the symbol name for testing purposes, but should be a
       unique string calculated from all the
        // available data, including date and time etc

        // bars are not in the cache
        DSMap::iterator i = _dataSources.find( di ->dataSourceId() );
        if ( i != _dataSources.end() )
        {
          const DataSource* ds = (*i).second->get();
          return _cache.findAndAdd( MakeData( di, ds, range ) );
        }
        else
        {
          // data source was not registered
          throw DataSourceException( DATA_SOURCE_NOT_REGISTERED_ERROR, "Data
       source not registered: ", di->dataSourceId() );
        }
            */
    return 0;
  }

  virtual void setCacheSize(unsigned int size) { _cache.setSize(size); }
};