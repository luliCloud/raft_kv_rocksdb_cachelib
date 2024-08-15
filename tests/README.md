This file is trashed. edited by Lu
#install gtest. The test CMakeList already indicated in top dir CMakeList file. 
#theoretically we don't need this file. Just leave it as a reference.
git clone https://github.com/google/googletest.git  // don't use this version. cannot make due to new-version template error. if you already install use the 
sudo rm -rf /usr/local/include/gtest /usr/local/lib/libgtest* to delete it

cd googletest
mkdir build
cd build
cmake ..
make
sudo make install

#validate install
ls /usr/local/lib | grep gtest
#output: 
libgtest.a
libgtest_main.a

If your project struct like this
project_root/
│
├── CMakeLists.txt
├── test/
│   ├── CMakeLists.txt
│   ├── test_msgpack.cpp
│   ├── test_bytebuffer.cpp
│   ├── ...

#check top dir
#if not exist this, adding for test
add_subdirectory(test) 

#check tests dir

