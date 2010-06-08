// RUN: %clang_cc1 -fsyntax-only -verify %s
template<typename T>
class X {
public:
  void f(T x); // expected-error{{argument may not have 'void' type}}
  void g(T*);

  static int h(T, T); // expected-error {{argument may not have 'void' type}}
};

int identity(int x) { return x; }

void test(X<int> *xi, int *ip, X<int(int)> *xf) {
  xi->f(17);
  xi->g(ip);
  xf->f(&identity);
  xf->g(identity);
  X<int>::h(17, 25);
  X<int(int)>::h(identity, &identity);
}

void test_bad() {
  X<void> xv; // expected-note{{in instantiation of template class 'X<void>' requested here}}
}

template<typename T, typename U>
class Overloading {
public:
  int& f(T, T); // expected-note{{previous declaration is here}}
  float& f(T, U); // expected-error{{functions that differ only in their return type cannot be overloaded}}
};

void test_ovl(Overloading<int, long> *oil, int i, long l) {
  int &ir = oil->f(i, i);
  float &fr = oil->f(i, l);
}

void test_ovl_bad() {
  Overloading<float, float> off; // expected-note{{in instantiation of template class 'Overloading<float, float>' requested here}}
}

template<typename T>
class HasDestructor {
public:
  virtual ~HasDestructor() = 0;
};

int i = sizeof(HasDestructor<int>); // FIXME: forces instantiation, but 
                // the code below should probably instantiate by itself.
int abstract_destructor[__is_abstract(HasDestructor<int>)? 1 : -1];


template<typename T>
class Constructors {
public:
  Constructors(const T&);
  Constructors(const Constructors &other);
};

void test_constructors() {
  Constructors<int> ci1(17);
  Constructors<int> ci2 = ci1;
}


template<typename T>
struct ConvertsTo {
  operator T();
};

void test_converts_to(ConvertsTo<int> ci, ConvertsTo<int *> cip) {
  int i = ci;
  int *ip = cip;
}

// PR4660
template<class T> struct A0 { operator T*(); };
template<class T> struct A1;

int *a(A0<int> &x0, A1<int> &x1) {
  int *y0 = x0;
  int *y1 = x1; // expected-error{{no viable conversion}}
}

struct X0Base {
  int &f();
  int& g(int);
  static double &g(double);
};

template<typename T>
struct X0 : X0Base {
};

template<typename U>
struct X1 : X0<U> {
  int &f2() { 
    return X0Base::f();
  }
};

void test_X1(X1<int> x1i) {
  int &ir = x1i.f2();
}

template<typename U>
struct X2 : X0Base, U {
  int &f2() { return X0Base::f(); }
};

template<typename T>
struct X3 {
  void test(T x) {
    double& d1 = X0Base::g(x);
  }
};


template struct X3<double>;

// Don't try to instantiate this, it's invalid.
namespace test1 {
  template <class T> class A {};
  template <class T> class B {
    void foo(A<test1::Undeclared> &a) // expected-error {{no member named 'Undeclared' in namespace 'test1'}}
    {}
  };
  template class B<int>;
}