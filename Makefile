test: build
	./build/bsk_test
	./build/bsk_dtest
sim: build
	./build/bsk_sim normal
	./build/bsk_sim degrade
	./build/bsk_sim recover
	./build/bsk_sim ratelimit
build:
	mkdir -p build
	g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude \
	    src/http.cpp src/cache.cpp src/service.cpp src/main.cpp -o build/bsk_sim
	g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude \
	    src/http.cpp src/cache.cpp src/service.cpp test/test_bsk.cpp -o build/bsk_test
	g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Iinclude \
	    src/data.cpp test/test_data.cpp -o build/bsk_dtest
clean:
	rm -rf build
.PHONY: test sim build clean
