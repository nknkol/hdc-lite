#!/bin/bash

ACTION=${1:-shell}

case "$ACTION" in
    build)
        ./build.sh
        ;;
    
    shell)
        echo "Starting development shell..."
        docker run --rm -it \
            -v $(pwd):/workspace \
            --name hdc-dev-shell \
            hdc-dev:latest \
            /bin/bash
        ;;
    
    compile)
        echo "Compiling..."
        docker run --rm -v $(pwd):/workspace hdc-dev:latest \
            bash -c "cd /workspace && make -f Makefile.simple clean && make -f Makefile.simple all"
        ;;
    
    test)
        echo "Testing hdc..."
        docker run --rm -v $(pwd):/workspace hdc-dev:latest \
            bash -c "cd /workspace && ./build/hdc -v && ./build/hdc -h"
        ;;
    
    server)
        echo "Starting HDC server..."
        docker run --rm -it \
            -v $(pwd):/workspace \
            -p 8710:8710 \
            --name hdc-server \
            hdc-dev:latest \
            /workspace/build/hdc start
        ;;
    
    *)
        echo "Usage: ./run.sh {build|shell|compile|test|server}"
        ;;
esac