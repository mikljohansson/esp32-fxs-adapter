#ifndef _PREFERENCES_SET_H_
#define _PREFERENCES_SET_H_

#include <Arduino.h>
#include <Preferences.h>

class PreferenceSet {
    private:
        Preferences &_preferences;
        String _key;

    public:
        PreferenceSet(Preferences &preferences, const char *key)
         : _preferences(preferences), _key(key) {}

        int size() {
            return _preferences.getInt((_key + ".c").c_str(), 0);
        }

        void add(const char *value) {
            if (!exists(value)) {
                int sz = size();
                _preferences.putString((_key + "." + sz).c_str(), value);
                _preferences.putInt((_key + ".c").c_str(), sz + 1);
            }
        }

        void remove(const char *value) {
            int sz = size();
            
            for (int i = 0; i < sz; i++) {
                if (get(i) == value) {
                    for (; i < sz; i++) {
                        _preferences.putString((_key + "." + i).c_str(), get(i + 1));
                    }

                    _preferences.putInt((_key + ".c").c_str(), sz - 1);
                    return;
                }
            }
        }

        void clear() {
            _preferences.putInt((_key + ".c").c_str(), 0);
        }

        String get(int index) {
            return _preferences.getString((_key + "." + index).c_str());
        }

        bool exists(const char *value) {
            for (int i = 0, sz = size(); i < sz; i++) {
                if (get(i) == value) {
                    return true;
                }
            }

            return false;
        }
};

#endif