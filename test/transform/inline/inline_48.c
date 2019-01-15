int foo(){
	return 42;
}

int main(){
	int i,j,k,x = 0;


#pragma spf transform inline
	for(i = 0; i < 32; i++){
		for(j = 0; j < 32; j++){
			for(k = 0; k < 32; k++){
				x += foo();
			}
		}
	}

	return 0;
} 
//CHECK: inline_48.c:9:23: warning: unexpected directive ignored
//CHECK: #pragma spf transform inline
//CHECK:                       ^
//CHECK: inline_48.c:10:2: note: no call suitable for inline is found
//CHECK:         for(i = 0; i < 32; i++){
//CHECK:         ^
//CHECK: 1 warning generated.
