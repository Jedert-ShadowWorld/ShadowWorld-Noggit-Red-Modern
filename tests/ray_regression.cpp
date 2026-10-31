#include <math/ray.hpp>
#include <math/sphere.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

void require(bool value, char const* message)
{
  if (!value) throw std::runtime_error(message);
}

void misses_all(math::ray const& ray)
{
  require(!ray.intersect_bounds(glm::vec3(-1.f), glm::vec3(1.f)), "Invalid ray hit bounds");
  require(!ray.intersect_triangle({-1.f,-1.f,5.f}, {1.f,-1.f,5.f}, {0.f,1.f,5.f}), "Invalid ray hit triangle");
  require(!ray.intersect_box(glm::vec3(0.f), glm::vec3(-1.f), glm::vec3(1.f), glm::vec3(0.f)), "Invalid ray hit box");
  require(!ray.intersects_sphere({{0.f,0.f,5.f},1.f}).hit, "Invalid ray hit sphere");
}

int main()
{
  try {
    auto nan = std::numeric_limits<float>::quiet_NaN();
    auto inf = std::numeric_limits<float>::infinity();
    misses_all({glm::vec3(0.f), glm::vec3(0.f)});
    misses_all({glm::vec3(0.f), {nan,0.f,1.f}});
    misses_all({glm::vec3(0.f), {inf,0.f,1.f}});
    misses_all({{nan,0.f,0.f}, {0.f,0.f,1.f}});
    misses_all({{inf,0.f,0.f}, {0.f,0.f,1.f}});
    math::ray original(glm::vec3(0.f), {0.f,0.f,1.f});
    auto singular = glm::inverse(glm::scale(glm::mat4(1.f), glm::vec3(0.f)));
    misses_all(math::ray(singular, original));
    misses_all(math::ray(glm::mat4(0.f), original));
    for (float size : {1.f, std::numeric_limits<float>::max(), std::numeric_limits<float>::min()}) {
      math::ray ray(glm::vec3(0.f), {0.f,0.f,size});
      require(std::abs(ray.position(1.f).z - 1.f) < 1.e-6f, "Valid direction did not normalize");
      auto hit = ray.intersect_triangle({-1.f,-1.f,5.f}, {1.f,-1.f,5.f}, {0.f,1.f,5.f});
      require(hit && std::abs(*hit - 5.f) < 1.e-5f, "Valid triangle picking changed");
      require(ray.intersect_bounds({-1.f,-1.f,4.f}, {1.f,1.f,6.f}).has_value(), "Valid bounds picking changed");
      require(ray.intersect_box({0.f,0.f,5.f}, glm::vec3(-1.f), glm::vec3(1.f), glm::vec3(0.f)).has_value(), "Valid box picking changed");
      require(ray.intersects_sphere({{0.f,0.f,5.f},1.f}).hit, "Valid sphere picking changed");
    }
    math::ray transformed(glm::inverse(glm::scale(glm::mat4(1.f), glm::vec3(2.f))), original);
    require(transformed.intersect_triangle({-1.f,-1.f,5.f}, {1.f,-1.f,5.f}, {0.f,1.f,5.f}).has_value(), "Valid transformed ray changed");
    std::cout << "PASS: zero/NaN/infinite rays and singular transforms miss safely; ordinary, huge, tiny and transformed rays pick correctly\n";
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
